// =============================================================================
// PhysicsWorld: Jolt setup, the fixed step, contact dispatch, interpolation,
// collision layers, static bodies and the mesh-collider cache.
// (Jolt-backed: see JoltInternal.hpp for the module's layout and its rule.)
// =============================================================================

#include "ve/physics/JoltInternal.hpp"

#include "ve/core/Log.hpp"
#include "ve/scene/Scene.hpp"

#include <Jolt/Core/Factory.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/RegisterTypes.h>

#include <cstdarg>
#include <cstdio>

namespace ve {

using namespace physics_detail;

namespace {

void joltTrace(const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    log::info("[jolt] {}", buf);
}

#ifdef JPH_ENABLE_ASSERTS
bool joltAssert(const char* expr, const char* msg, const char* file, JPH::uint line)
{
    log::error("[jolt] assert {} ({}) at {}:{}", expr, msg ? msg : "", file, line);
    return true; // break into the debugger
}
#endif

} // namespace

// --- Helpers shared by the module (declared in JoltInternal.hpp) ---------------------

namespace physics_detail {

BodyOwner ownerOf(JPH::uint64 ud)
{
    BodyOwner o;
    switch (ud & 3u) {
    case 1: o.userValue = static_cast<uint32_t>(ud >> 2); break;
    case 2: o.gameObject = &reinterpret_cast<CharacterController*>(ud & ~JPH::uint64(3))->gameObject(); break;
    default:
        o.body = reinterpret_cast<Rigidbody*>(ud);
        if (o.body) o.gameObject = &o.body->gameObject();
        break;
    }
    return o;
}

std::string surfaceOf(const JPH::Body& body)
{
    const JPH::uint64 ud = body.GetUserData();
    switch (ud & 3u) {
    case 1: {
        auto& impl = Engine::get().physics().impl();
        auto it = impl.staticSurfaces.find(body.GetID().GetIndexAndSequenceNumber());
        return it != impl.staticSurfaces.end() ? impl.surfaceNames[it->second] : std::string();
    }
    case 2: return reinterpret_cast<CharacterController*>(ud & ~JPH::uint64(3))->surface;
    default: return ud ? reinterpret_cast<Rigidbody*>(ud)->surface : std::string();
    }
}

int resolveLayer(const std::string& name, const std::string& who)
{
    if (name.empty() || name == "Default") return 0;
    const int l = Engine::get().physics().layer(name);
    if (l < 0) log::warn("Physics: '{}' asks for collision layer '{}', which wasn't added (PhysicsWorld::addLayer)", who, name);
    return std::max(l, 0);
}

} // namespace physics_detail

// =============================================================================
// PhysicsWorld
// =============================================================================

PhysicsWorld::PhysicsWorld() : m_impl(std::make_unique<Impl>()) {}

PhysicsWorld::~PhysicsWorld()
{
    shutdown();
}

void PhysicsWorld::init()
{
    // Global Jolt setup: allocator, logging, and the type factory used to
    // create shapes.
    JPH::RegisterDefaultAllocator();
    JPH::Trace = joltTrace;
    JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = joltAssert;)
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
    g_joltInitialized = true;

    // Scratch memory for each step, and worker threads for the solver.
    m_impl->temp = std::make_unique<JPH::TempAllocatorImpl>(16 * 1024 * 1024);
    const int threads = static_cast<int>(jobs::physicsThreadCount()); // (Jolt keeps its own pool: see Jobs.hpp)
    m_impl->jobs = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, threads);

    m_impl->objectPairs.table = &m_impl->layers;
    m_impl->system = std::make_unique<JPH::PhysicsSystem>();
    m_impl->system->Init(/*maxBodies*/ 65536, /*bodyMutexes (0 = default)*/ 0, /*maxBodyPairs*/ 65536,
                         /*maxContactConstraints*/ 16384, m_impl->broadPhaseLayers, m_impl->objectVsBroadPhase,
                         m_impl->objectPairs);
    m_impl->system->SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));
    m_impl->system->SetContactListener(&m_impl->contacts);
    log::info("Physics: Jolt {}.{}.{}, {} worker threads", JPH_VERSION_MAJOR, JPH_VERSION_MINOR, JPH_VERSION_PATCH,
              threads);
}

void PhysicsWorld::shutdown()
{
    if (!m_impl->system) return;
    m_impl->debugTriangles.clear(); // (the shapes they hold go before Jolt's factory)
    m_meshColliders.reset();         // (likewise)
    m_impl->system.reset();
    m_impl->jobs.reset();
    m_impl->temp.reset();
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
    g_joltInitialized = false;
}

glm::vec3 PhysicsWorld::gravity() const
{
    return toG(m_impl->system->GetGravity());
}

void PhysicsWorld::setGravity(const glm::vec3& g)
{
    m_impl->system->SetGravity(toJ(g));
}

uint32_t PhysicsWorld::bodyCount() const
{
    return m_impl->system ? m_impl->system->GetNumBodies() : 0;
}

void PhysicsWorld::step(Scene& scene, float dt)
{
    if (paused || !m_impl->system) return;
    JPH::BodyInterface& bi = m_impl->system->GetBodyInterface();

    // Kinematic bodies follow their Transform (moved by gameplay code); Jolt
    // computes the velocity needed so they push dynamic bodies correctly.
    scene.forEach<Rigidbody>([&](Rigidbody& rb) {
        if (rb.created() && rb.type == BodyType::Kinematic) {
            bi.MoveKinematic(JPH::BodyID(rb.bodyId()), toJ(rb.transform().position()), toJ(rb.transform().rotation()), dt);
        }
    });

    // After adding lots of bodies (level load), rebuild the broad phase trees once.
    if (m_impl->addedSinceOptimize > 256) {
        m_impl->system->OptimizeBroadPhase();
        m_impl->addedSinceOptimize = 0;
    }

    m_impl->system->Update(dt, /*collisionSteps*/ 1, m_impl->temp.get(), m_impl->jobs.get());
    // Joint forces this step (and the ones that break).
    // (Over a copy: a joint that breaks detaches, which erases it from the list.
    // The scratch vector keeps its capacity, so no allocation per step.)
    m_impl->jointScratch.assign(m_impl->joints.begin(), m_impl->joints.end());
    for (Joint* j : m_impl->jointScratch) j->afterStep(dt);

    // Remember the last two poses of each dynamic body for interpolation.
    scene.forEach<Rigidbody>([&](Rigidbody& rb) {
        if (!rb.created() || rb.type != BodyType::Dynamic) return;
        JPH::Vec3 p;
        JPH::Quat q;
        bi.GetPositionAndRotation(JPH::BodyID(rb.bodyId()), p, q);
        rb.m_prevPos = rb.m_currPos;
        rb.m_prevRot = rb.m_currRot;
        rb.m_currPos = toG(p);
        rb.m_currRot = toG(q);
    });

    m_impl->dispatchContacts();
}

namespace {

void sendCollision(GameObject* to, void (Behaviour::*hook)(const Collision&), const Collision& c)
{
    if (!to || to->isDestroyed()) return;
    (to->*hook)(c);
    // Index loop: a handler may add components.
    for (size_t i = 0; i < to->components().size(); ++i) {
        Component* comp = to->components()[i].get();
        if (comp->enabled) (comp->*hook)(c);
    }
}

void sendTrigger(GameObject* to, GameObject* other, bool enter)
{
    if (!to || !other || to->isDestroyed() || other->isDestroyed()) return;
    enter ? to->onTriggerEnter(*other) : to->onTriggerExit(*other);
    for (size_t i = 0; i < to->components().size(); ++i) {
        Component* comp = to->components()[i].get();
        if (!comp->enabled) continue;
        enter ? comp->onTriggerEnter(*other) : comp->onTriggerExit(*other);
    }
}

} // namespace

void PhysicsWorld::Impl::dispatchContacts()
{
    {
        std::lock_guard lock(contacts.mutex);
        scratch.swap(contacts.events);
        contacts.events.clear();
    }
    if (scratch.empty() && pairs.empty()) return;

    std::vector<ContactOut>& out = contactOut;
    out.clear();
    auto key = [](JPH::BodyID a, JPH::BodyID b) {
        return (uint64_t(a.GetIndexAndSequenceNumber()) << 32) | b.GetIndexAndSequenceNumber();
    };
    for (const ContactQueue::Event& e : scratch) {
        const uint64_t k = key(e.a, e.b);
        if (e.kind == ContactQueue::Kind::Removed) {
            auto it = pairs.find(k);
            if (it == pairs.end()) continue;
            if (--it->second.count <= 0) {
                out.push_back({Phase::Exit, it->second.sensor, e.a, e.b, it->second.point, it->second.normal, 0.0f});
                pairs.erase(it);
            }
            continue;
        }
        Pair& p = pairs[k];
        p.sensor = p.sensor || e.sensor;
        p.point = e.point;
        p.normal = e.normal;
        if (e.kind == ContactQueue::Kind::Added) {
            if (p.count++ == 0) {
                out.push_back({Phase::Enter, p.sensor, e.a, e.b, e.point, e.normal, e.speed});
                p.enteredNow = true;
            }
        } else {
            p.persisted = true;
        }
    }
    scratch.clear();
    for (auto& [k, p] : pairs) {
        if (p.persisted && !p.sensor && !p.enteredNow) {
            out.push_back({Phase::Stay, false, JPH::BodyID(uint32_t(k >> 32)), JPH::BodyID(uint32_t(k)), p.point, p.normal, 0.0f});
        }
        p.persisted = false;
        p.enteredNow = false;
    }
    if (out.empty()) return;

    // Resolve owners now (main thread, outside the update); removed bodies resolve to nothing.
    const JPH::BodyLockInterfaceNoLock& locks = system->GetBodyLockInterfaceNoLock();
    struct Side {
        BodyOwner owner;
        int layer = 0;
        std::string surface;
    };
    // (Triggers don't report surfaces: no string for them.)
    auto side = [&](JPH::BodyID id, bool withSurface) {
        JPH::BodyLockRead lock(locks, id);
        if (!lock.Succeeded()) return Side{};
        const JPH::Body& body = lock.GetBody();
        return Side{ownerOf(body.GetUserData()), gameLayerOf(body.GetObjectLayer()), withSurface ? surfaceOf(body) : std::string()};
    };
    for (const ContactOut& o : out) {
        Side sa = side(o.a, !o.sensor), sb = side(o.b, !o.sensor);
        const BodyOwner &a = sa.owner, &b = sb.owner;
        if (!a.gameObject && !b.gameObject) continue;
        if (o.sensor) {
            if (o.phase == Phase::Stay) continue;
            sendTrigger(a.gameObject, b.gameObject, o.phase == Phase::Enter);
            sendTrigger(b.gameObject, a.gameObject, o.phase == Phase::Enter);
            continue;
        }
        void (Behaviour::*hook)(const Collision&) = &Behaviour::onCollisionExit;
        if (o.phase == Phase::Enter) hook = &Behaviour::onCollisionEnter;
        else if (o.phase == Phase::Stay) hook = &Behaviour::onCollisionStay;
        // (The surface names move into the Collisions: one copy each, made by surfaceOf.)
        Collision ca{b.gameObject, b.body, b.userValue, o.point, -o.normal, o.speed, sb.layer, std::move(sb.surface)}; // seen from a
        Collision cb{a.gameObject, a.body, a.userValue, o.point, o.normal, o.speed, sa.layer, std::move(sa.surface)};  // seen from b
        sendCollision(a.gameObject, hook, ca);
        sendCollision(b.gameObject, hook, cb);
    }
}

void PhysicsWorld::interpolate(Scene& scene, float alpha)
{
    scene.forEach<Rigidbody>([&](Rigidbody& rb) {
        if (!rb.created() || rb.type != BodyType::Dynamic) return;
        // Resting (asleep) bodies: already in place, don't dirty their transforms.
        if (rb.m_prevPos == rb.m_currPos && rb.m_prevRot == rb.m_currRot && rb.transform().position() == rb.m_currPos &&
            rb.transform().rotation() == rb.m_currRot)
            return;
        const float a = rb.interpolate ? alpha : 1.0f;
        rb.transform().setPosition(glm::mix(rb.m_prevPos, rb.m_currPos, a));
        rb.transform().setRotation(glm::slerp(rb.m_prevRot, rb.m_currRot, a));
    });
    scene.forEach<CharacterController>([&](CharacterController& cc) {
        if (!cc.isActive()) return;
        const float a = cc.interpolate ? alpha : 1.0f;
        cc.transform().setPosition(glm::mix(cc.m_prevPos, cc.m_currPos, a));
    });
}

// --- Static bodies (no GameObject: terrain tiles, foliage) ------------------------------

uint32_t PhysicsWorld::createStaticBody(const Collider& collider, const glm::vec3& position, const glm::quat& rotation,
                                       const glm::vec3& scale, uint32_t userValue, const std::string& surface, int layer)
{
    if (!collider.valid()) return Rigidbody::kInvalidBody;
    JPH::BodyCreationSettings s(scaledShape(collider, scale), toJ(position), toJ(rotation), JPH::EMotionType::Static,
                                makeLayer(Layers::NonMoving, std::clamp(layer, 0, kMaxLayers - 1)));
    s.mUserData = staticUserData(userValue);
    const JPH::BodyID id = m_impl->system->GetBodyInterface().CreateAndAddBody(s, JPH::EActivation::DontActivate);
    if (id.IsInvalid()) return Rigidbody::kInvalidBody;
    ++m_impl->addedSinceOptimize;
    if (!surface.empty()) m_impl->staticSurfaces[id.GetIndexAndSequenceNumber()] = m_impl->surfaceId(surface);
    return id.GetIndexAndSequenceNumber();
}

void PhysicsWorld::destroyStaticBody(uint32_t bodyId)
{
    if (bodyId == Rigidbody::kInvalidBody || !m_impl->system) return;
    JPH::BodyInterface& bi = m_impl->system->GetBodyInterface();
    bi.RemoveBody(JPH::BodyID(bodyId));
    bi.DestroyBody(JPH::BodyID(bodyId));
    m_impl->staticSurfaces.erase(bodyId);
}

// --- Collision layers ---------------------------------------------------------------

int PhysicsWorld::addLayer(const std::string& name)
{
    auto& names = m_impl->layers.names;
    for (size_t i = 0; i < names.size(); ++i)
        if (names[i] == name) return int(i);
    if (names.size() >= size_t(kMaxLayers)) {
        log::error("Physics: no room for collision layer '{}' (max {})", name, kMaxLayers);
        return -1;
    }
    names.push_back(name);
    return int(names.size() - 1);
}

int PhysicsWorld::layer(const std::string& name) const
{
    const auto& names = m_impl->layers.names;
    for (size_t i = 0; i < names.size(); ++i)
        if (names[i] == name) return int(i);
    return -1;
}

const std::string& PhysicsWorld::layerName(int index) const
{
    static const std::string none;
    const auto& names = m_impl->layers.names;
    return index >= 0 && size_t(index) < names.size() ? names[size_t(index)] : none;
}

int PhysicsWorld::layerCount() const
{
    return int(m_impl->layers.names.size());
}

void PhysicsWorld::setCollision(int a, int b, bool collide)
{
    if (a < 0 || b < 0 || a >= kMaxLayers || b >= kMaxLayers) return;
    uint32_t* m = m_impl->layers.collide;
    if (collide) {
        m[a] |= 1u << b;
        m[b] |= 1u << a;
    } else {
        m[a] &= ~(1u << b);
        m[b] &= ~(1u << a);
    }
}

bool PhysicsWorld::collides(int a, int b) const
{
    return a >= 0 && b >= 0 && a < kMaxLayers && b < kMaxLayers && m_impl->layers.pair(a, b);
}

uint32_t PhysicsWorld::layerMask(std::initializer_list<const char*> names) const
{
    uint32_t mask = 0;
    for (const char* n : names)
        if (const int l = layer(n); l >= 0) mask |= 1u << l;
    return mask;
}

// --- Mesh collider cache ------------------------------------------------------------

// Keyed by the mesh's address; the weak_ptr tells a live mesh from a new one
// that reuses a freed one's address. (It used to live in the Model, so the
// assets module had to include physics.) Locked, as it's shared engine state.
struct PhysicsWorld::MeshColliderCache {
    struct Entry {
        std::weak_ptr<const Mesh> mesh;
        Collider collider;
    };
    std::mutex mutex;
    std::unordered_map<const Mesh*, Entry> entries;
};

Collider PhysicsWorld::meshCollider(const std::shared_ptr<const Mesh>& mesh, const MeshData& data)
{
    if (!mesh) return Collider::mesh(data);
    if (!m_meshColliders) m_meshColliders = std::make_shared<MeshColliderCache>();
    MeshColliderCache& cache = *m_meshColliders;
    std::lock_guard lock(cache.mutex);
    if (auto it = cache.entries.find(mesh.get()); it != cache.entries.end() && !it->second.mesh.expired())
        return it->second.collider;
    // A new mesh: also forget the meshes that are gone (bodies keep their own
    // references to the shapes).
    std::erase_if(cache.entries, [](const auto& kv) { return kv.second.mesh.expired(); });
    auto& e = cache.entries[mesh.get()];
    e = {mesh, Collider::mesh(data)};
    return e.collider;
}

} // namespace ve
