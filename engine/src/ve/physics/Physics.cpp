// =============================================================================
// Jolt Physics backend for ve/physics/Physics.hpp.
//
// Everything Jolt-specific lives in this file. Key Jolt concepts used here:
//   PhysicsSystem       the world: bodies, broad phase, constraints
//   ObjectLayer         what kind of object a body is (static, moving, character)
//   BroadPhaseLayer     coarse acceleration-structure buckets (static vs moving)
//   Shape               immutable, ref-counted collision geometry (our Collider)
//   BodyInterface       thread-safe API to create/move/query bodies by BodyID
//   CharacterVirtual    a kinematic capsule moved by queries (our CharacterController)
// =============================================================================

#include "ve/physics/Physics.hpp"

#include "ve/core/Engine.hpp"
#include "ve/core/Log.hpp"
#include "ve/render/Mesh.hpp"
#include "ve/scene/Scene.hpp"

// Jolt.h must come first.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace ve {

namespace {

// ---- glm <-> Jolt conversions -----------------------------------------------------
JPH::Vec3 toJ(const glm::vec3& v) { return JPH::Vec3(v.x, v.y, v.z); }
JPH::Quat toJ(const glm::quat& q) { return JPH::Quat(q.x, q.y, q.z, q.w); }
glm::vec3 toG(JPH::Vec3Arg v) { return {v.GetX(), v.GetY(), v.GetZ()}; }
glm::quat toG(JPH::QuatArg q) { return glm::quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ()); }

// ---- Collision layers ------------------------------------------------------------
// Object layers say what collides with what; broad-phase layers group bodies
// into separate acceleration trees (static bodies rarely change, so they get
// their own tree that's cheap to keep optimised).
namespace Layers {
constexpr JPH::ObjectLayer NonMoving = 0; // static bodies
constexpr JPH::ObjectLayer Moving = 1;    // dynamic + kinematic bodies
constexpr JPH::ObjectLayer Character = 2; // the capsule that lets bodies hit characters
constexpr JPH::ObjectLayer Sensor = 3;    // trigger volumes (Rigidbody::isTrigger)
} // namespace Layers
namespace BPLayers {
constexpr JPH::BroadPhaseLayer NonMoving(0);
constexpr JPH::BroadPhaseLayer Moving(1);
constexpr uint32_t Count = 2;
} // namespace BPLayers

class BroadPhaseLayerMap final : public JPH::BroadPhaseLayerInterface {
public:
    uint32_t GetNumBroadPhaseLayers() const override { return BPLayers::Count; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override
    {
        return layer == Layers::NonMoving ? BPLayers::NonMoving : BPLayers::Moving;
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override
    {
        return layer == BPLayers::NonMoving ? "NonMoving" : "Moving";
    }
#endif
};

class ObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer bp) const override
    {
        // Static things, characters and triggers only need to test against moving things.
        if (layer == Layers::Moving) return true;
        return bp == BPLayers::Moving;
    }
};

class ObjectPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override
    {
        if (a == Layers::NonMoving) return b == Layers::Moving;
        if (a == Layers::Moving) return true;
        if (a == Layers::Character) return b == Layers::Moving || b == Layers::Sensor; // hit by bodies, enters triggers
        return b == Layers::Moving || b == Layers::Character;                         // triggers
    }
};

// Query filter by layer (QueryFilter's statics / dynamics / characters / triggers).
class QueryLayerFilter final : public JPH::ObjectLayerFilter {
public:
    explicit QueryLayerFilter(const QueryFilter& f) : m_f(f) {}
    bool ShouldCollide(JPH::ObjectLayer layer) const override
    {
        switch (layer) {
        case Layers::NonMoving: return m_f.statics;
        case Layers::Moving: return m_f.dynamics;
        case Layers::Character: return m_f.characters;
        case Layers::Sensor: return m_f.triggers;
        default: return false;
        }
    }

private:
    const QueryFilter& m_f;
};

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

bool g_joltInitialized = false;

} // namespace

struct Collider::Impl {
    JPH::RefConst<JPH::Shape> shape;
};

// =============================================================================
// PhysicsWorld
// =============================================================================

// Collects contact callbacks (from Jolt's worker threads) for main-thread dispatch.
class ContactQueue final : public JPH::ContactListener {
public:
    enum class Kind : uint8_t { Added, Persisted, Removed };
    struct Event {
        Kind kind;
        bool sensor;
        JPH::BodyID a, b; // a < b
        glm::vec3 point{0.0f};
        glm::vec3 normal{0.0f}; // from a to b
        float speed = 0.0f;
    };

    void OnContactAdded(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& m, JPH::ContactSettings&) override
    {
        push(Kind::Added, a, b, m);
    }
    void OnContactPersisted(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& m,
                            JPH::ContactSettings&) override
    {
        push(Kind::Persisted, a, b, m);
    }
    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override
    {
        std::lock_guard lock(mutex);
        events.push_back({Kind::Removed, false, pair.GetBody1ID(), pair.GetBody2ID()});
    }

    std::mutex mutex;
    std::vector<Event> events;

private:
    void push(Kind kind, const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& m)
    {
        Event e{kind, a.IsSensor() || b.IsSensor(), a.GetID(), b.GetID()};
        const JPH::RVec3 p = m.GetWorldSpaceContactPointOn1(0);
        e.point = glm::vec3(float(p.GetX()), float(p.GetY()), float(p.GetZ()));
        e.normal = toG(m.mWorldSpaceNormal);
        if (kind == Kind::Added) {
            // Closing speed before the solver runs (bodies are locked: no lock calls here).
            const JPH::Vec3 rel = b.GetPointVelocity(p) - a.GetPointVelocity(p);
            e.speed = std::max(0.0f, -rel.Dot(m.mWorldSpaceNormal));
        }
        std::lock_guard lock(mutex);
        events.push_back(e);
    }
};

struct PhysicsWorld::Impl {
    std::unique_ptr<JPH::TempAllocatorImpl> temp;
    std::unique_ptr<JPH::JobSystemThreadPool> jobs;
    BroadPhaseLayerMap broadPhaseLayers;
    ObjectVsBroadPhaseFilter objectVsBroadPhase;
    ObjectPairFilter objectPairs;
    std::unique_ptr<JPH::PhysicsSystem> system;
    uint32_t addedSinceOptimize = 0;

    // Contact pairs (body level; Jolt reports sub-shape pairs, so count them).
    ContactQueue contacts;
    struct Pair {
        int count = 0;
        bool sensor = false;
        bool persisted = false;
        glm::vec3 point{0.0f}, normal{0.0f};
    };
    std::unordered_map<uint64_t, Pair> pairs;
    std::vector<ContactQueue::Event> scratch;

    void dispatchContacts();
};

namespace {
JPH::BodyInterface& bodies()
{
    return Engine::get().physics().impl().system->GetBodyInterface();
}

bool physicsAlive()
{
    return g_joltInitialized && Engine::exists() && Engine::get().physics().impl().system;
}

// Body user data, tagged in the low 2 bits (pointers are 8-byte aligned):
//   0: Rigidbody*   1: (value << 2) for GameObject-less static bodies
//   2: CharacterController* (its inner body)
JPH::uint64 staticUserData(uint32_t value)
{
    return (JPH::uint64(value) << 2) | 1u;
}

JPH::uint64 characterUserData(const CharacterController* c)
{
    return reinterpret_cast<JPH::uint64>(c) | 2u;
}

struct BodyOwner {
    GameObject* gameObject = nullptr;
    Rigidbody* body = nullptr;
    uint32_t userValue = 0;
};

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

// Filters a query's bodies: the ignored body / character.
class QueryBodyFilter final : public JPH::BodyFilter {
public:
    explicit QueryBodyFilter(const QueryFilter& f, JPH::BodyID character) : m_f(f), m_character(character) {}
    bool ShouldCollide(const JPH::BodyID& id) const override
    {
        if (m_f.ignore && m_f.ignore->created() && id == JPH::BodyID(m_f.ignore->bodyId())) return false;
        return m_character.IsInvalid() || id != m_character;
    }

private:
    const QueryFilter& m_f;
    JPH::BodyID m_character;
};

} // namespace

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
    const int threads = std::clamp(static_cast<int>(std::thread::hardware_concurrency()) - 1, 1, 4);
    m_impl->jobs = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, threads);

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

    enum class Phase { Enter, Stay, Exit };
    struct Out {
        Phase phase;
        bool sensor;
        JPH::BodyID a, b;
        glm::vec3 point, normal;
        float speed;
    };
    std::vector<Out> out;
    auto key = [](JPH::BodyID a, JPH::BodyID b) {
        return (uint64_t(a.GetIndexAndSequenceNumber()) << 32) | b.GetIndexAndSequenceNumber();
    };
    std::vector<uint64_t> entered;
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
                entered.push_back(k);
            }
        } else {
            p.persisted = true;
        }
    }
    scratch.clear();
    for (auto& [k, p] : pairs) {
        if (p.persisted && !p.sensor && std::find(entered.begin(), entered.end(), k) == entered.end()) {
            out.push_back({Phase::Stay, false, JPH::BodyID(uint32_t(k >> 32)), JPH::BodyID(uint32_t(k)), p.point, p.normal, 0.0f});
        }
        p.persisted = false;
    }
    if (out.empty()) return;

    // Resolve owners now (main thread, outside the update); removed bodies resolve to nothing.
    const JPH::BodyLockInterfaceNoLock& locks = system->GetBodyLockInterfaceNoLock();
    auto owner = [&](JPH::BodyID id) {
        JPH::BodyLockRead lock(locks, id);
        return lock.Succeeded() ? ownerOf(lock.GetBody().GetUserData()) : BodyOwner{};
    };
    for (const Out& o : out) {
        const BodyOwner a = owner(o.a), b = owner(o.b);
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
        Collision ca{b.gameObject, b.body, b.userValue, o.point, -o.normal, o.speed}; // seen from a
        Collision cb{a.gameObject, a.body, a.userValue, o.point, o.normal, o.speed};  // seen from b
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

std::optional<RaycastHit> PhysicsWorld::raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
                                                const Rigidbody* ignore, bool hitCharacters) const
{
    QueryFilter f;
    f.ignore = ignore;
    f.characters = hitCharacters;
    return raycast(origin, direction, maxDistance, f);
}

namespace {

JPH::BodyID characterBody(const QueryFilter& f)
{
    return f.ignoreCharacter ? JPH::BodyID(f.ignoreCharacter->innerBodyId()) : JPH::BodyID();
}

// Fills body / gameObject / userValue (and the normal, when asked) from a body.
void fillHit(const JPH::PhysicsSystem& system, JPH::BodyID id, const JPH::SubShapeID& subShape, RaycastHit& hit,
             bool surfaceNormal)
{
    JPH::BodyLockRead lock(system.GetBodyLockInterface(), id);
    if (!lock.Succeeded()) return;
    const JPH::Body& body = lock.GetBody();
    if (surfaceNormal) hit.normal = toG(body.GetWorldSpaceSurfaceNormal(subShape, toJ(hit.point)));
    const BodyOwner o = ownerOf(body.GetUserData());
    hit.body = o.body;
    hit.gameObject = o.gameObject;
    hit.userValue = o.userValue;
}

} // namespace

std::optional<RaycastHit> PhysicsWorld::raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
                                                const QueryFilter& filter) const
{
    if (!m_impl->system || maxDistance <= 0.0f || glm::dot(direction, direction) < 1e-12f) return std::nullopt;
    const glm::vec3 dir = glm::normalize(direction);
    const JPH::RRayCast ray{toJ(origin), toJ(dir * maxDistance)};
    JPH::RayCastResult hit;
    QueryLayerFilter layerFilter(filter);
    QueryBodyFilter bodyFilter(filter, characterBody(filter));
    if (!m_impl->system->GetNarrowPhaseQuery().CastRay(ray, hit, {}, layerFilter, bodyFilter)) return std::nullopt;

    RaycastHit result;
    result.distance = hit.mFraction * maxDistance;
    result.point = origin + dir * result.distance;
    fillHit(*m_impl->system, hit.mBodyID, hit.mSubShapeID2, result, true);
    return result;
}

std::optional<RaycastHit> PhysicsWorld::sphereCast(const glm::vec3& origin, float radius, const glm::vec3& direction,
                                                   float maxDistance, const QueryFilter& filter) const
{
    return shapeCast(Collider::sphere(std::max(radius, 1e-3f)), origin, glm::quat(1, 0, 0, 0), direction, maxDistance, filter);
}

std::optional<RaycastHit> PhysicsWorld::boxCast(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation,
                                                const glm::vec3& direction, float maxDistance, const QueryFilter& filter) const
{
    return shapeCast(Collider::box(glm::max(halfExtents, glm::vec3(1e-3f))), center, rotation, direction, maxDistance, filter);
}

std::optional<RaycastHit> PhysicsWorld::shapeCast(const Collider& shape, const glm::vec3& position, const glm::quat& rotation,
                                                  const glm::vec3& direction, float maxDistance, const QueryFilter& filter) const
{
    if (!m_impl->system || !shape.valid() || maxDistance < 0.0f || glm::dot(direction, direction) < 1e-12f) return std::nullopt;
    const glm::vec3 dir = glm::normalize(direction);
    const JPH::RMat44 start = JPH::RMat44::sRotationTranslation(toJ(rotation), toJ(position));
    const JPH::RShapeCast cast = JPH::RShapeCast::sFromWorldTransform(shape.impl->shape, JPH::Vec3::sReplicate(1.0f), start,
                                                                      toJ(dir * maxDistance));
    JPH::ShapeCastSettings settings;
    settings.mReturnDeepestPoint = true; // starts inside: report the way out
    settings.mBackFaceModeTriangles = JPH::EBackFaceMode::IgnoreBackFaces;
    settings.mBackFaceModeConvex = JPH::EBackFaceMode::CollideWithBackFaces;
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    QueryLayerFilter layerFilter(filter);
    QueryBodyFilter bodyFilter(filter, characterBody(filter));
    m_impl->system->GetNarrowPhaseQuery().CastShape(cast, settings, JPH::RVec3::sZero(), collector, {}, layerFilter, bodyFilter);
    if (!collector.HadHit()) return std::nullopt;

    const JPH::ShapeCastResult& h = collector.mHit;
    RaycastHit result;
    result.distance = h.mFraction * maxDistance;
    result.point = toG(h.mContactPointOn2);
    // The penetration axis points from the cast shape into what it hit.
    const glm::vec3 axis = toG(h.mPenetrationAxis);
    result.normal = glm::dot(axis, axis) > 1e-12f ? -glm::normalize(axis) : -dir;
    fillHit(*m_impl->system, h.mBodyID2, h.mSubShapeID2, result, false);
    return result;
}

std::vector<RaycastHit> PhysicsWorld::overlapSphere(const glm::vec3& center, float radius, const QueryFilter& filter) const
{
    return overlap(Collider::sphere(std::max(radius, 1e-3f)), center, glm::quat(1, 0, 0, 0), filter);
}

std::vector<RaycastHit> PhysicsWorld::overlapBox(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation,
                                                 const QueryFilter& filter) const
{
    return overlap(Collider::box(glm::max(halfExtents, glm::vec3(1e-3f))), center, rotation, filter);
}

std::vector<RaycastHit> PhysicsWorld::overlap(const Collider& shape, const glm::vec3& position, const glm::quat& rotation,
                                              const QueryFilter& filter) const
{
    std::vector<RaycastHit> hits;
    if (!m_impl->system || !shape.valid()) return hits;
    const JPH::Shape& s = *shape.impl->shape;
    // CollideShape wants the centre-of-mass transform.
    const JPH::RMat44 com = JPH::RMat44::sRotationTranslation(toJ(rotation), toJ(position)).PreTranslated(s.GetCenterOfMass());
    JPH::CollideShapeSettings settings;
    settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    QueryLayerFilter layerFilter(filter);
    QueryBodyFilter bodyFilter(filter, characterBody(filter));
    m_impl->system->GetNarrowPhaseQuery().CollideShape(&s, JPH::Vec3::sReplicate(1.0f), com, settings, JPH::RVec3::sZero(),
                                                       collector, {}, layerFilter, bodyFilter);
    // One hit per body: the deepest contact.
    std::unordered_map<uint32_t, size_t> byBody;
    std::vector<float> depth;
    for (const JPH::CollideShapeResult& r : collector.mHits) {
        const uint32_t id = r.mBodyID2.GetIndexAndSequenceNumber();
        auto [it, inserted] = byBody.try_emplace(id, hits.size());
        if (!inserted && r.mPenetrationDepth <= depth[it->second]) continue;
        RaycastHit h;
        h.point = toG(r.mContactPointOn2);
        const glm::vec3 axis = toG(r.mPenetrationAxis);
        h.normal = glm::dot(axis, axis) > 1e-12f ? -glm::normalize(axis) : glm::vec3(0, 1, 0);
        fillHit(*m_impl->system, r.mBodyID2, r.mSubShapeID2, h, false);
        if (inserted) {
            hits.push_back(h);
            depth.push_back(r.mPenetrationDepth);
        } else {
            hits[it->second] = h;
            depth[it->second] = r.mPenetrationDepth;
        }
    }
    return hits;
}

// =============================================================================
// Collider
// =============================================================================

namespace {

Collider makeCollider(const JPH::ShapeSettings::ShapeResult& result, const char* what)
{
    if (result.HasError()) {
        throw std::runtime_error(std::string("Collider::") + what + ": " + result.GetError().c_str());
    }
    auto impl = std::make_shared<Collider::Impl>();
    impl->shape = result.Get();
    Collider c;
    c.impl = impl;
    return c;
}

// The collider's shape with a world scale baked in (Jolt ScaledShape).
JPH::RefConst<JPH::Shape> scaledShape(const Collider& collider, const glm::vec3& scale)
{
    JPH::RefConst<JPH::Shape> shape = collider.impl->shape;
    if (glm::any(glm::greaterThan(glm::abs(scale - 1.0f), glm::vec3(1e-4f)))) {
        shape = new JPH::ScaledShape(shape, shape->MakeScaleValid(toJ(scale)));
    }
    return shape;
}

void requireJolt(const char* what)
{
    if (!g_joltInitialized) {
        throw std::runtime_error(std::string("Collider::") + what + " called before the Engine (physics) was created");
    }
}

} // namespace

Collider Collider::box(const glm::vec3& halfExtents)
{
    requireJolt("box");
    // Convex radius rounds the corners slightly (faster, more stable collisions)
    // and must be smaller than the half extents.
    const float radius = std::min(JPH::cDefaultConvexRadius, glm::min(halfExtents.x, glm::min(halfExtents.y, halfExtents.z)) * 0.5f);
    return makeCollider(JPH::BoxShapeSettings(toJ(halfExtents), radius).Create(), "box");
}

Collider Collider::sphere(float radius)
{
    requireJolt("sphere");
    return makeCollider(JPH::SphereShapeSettings(radius).Create(), "sphere");
}

Collider Collider::capsule(float halfHeight, float radius)
{
    requireJolt("capsule");
    return makeCollider(JPH::CapsuleShapeSettings(halfHeight, radius).Create(), "capsule");
}

Collider Collider::cylinder(float halfHeight, float radius)
{
    requireJolt("cylinder");
    const float convex = std::min(JPH::cDefaultConvexRadius, std::min(halfHeight, radius) * 0.5f);
    return makeCollider(JPH::CylinderShapeSettings(halfHeight, radius, convex).Create(), "cylinder");
}

Collider Collider::convexHull(const std::vector<glm::vec3>& points)
{
    requireJolt("convexHull");
    JPH::Array<JPH::Vec3> pts;
    pts.reserve(points.size());
    for (const auto& p : points) pts.push_back(toJ(p));
    return makeCollider(JPH::ConvexHullShapeSettings(pts).Create(), "convexHull");
}

Collider Collider::convexHull(const MeshData& mesh)
{
    std::vector<glm::vec3> pts;
    pts.reserve(mesh.vertices.size());
    for (const auto& v : mesh.vertices) pts.push_back(v.position);
    return convexHull(pts);
}

Collider Collider::mesh(const MeshData& mesh)
{
    requireJolt("mesh");
    JPH::VertexList verts;
    verts.reserve(mesh.vertices.size());
    for (const auto& v : mesh.vertices) verts.push_back(JPH::Float3(v.position.x, v.position.y, v.position.z));
    JPH::IndexedTriangleList tris;
    tris.reserve(mesh.indices.size() / 3);
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        tris.push_back(JPH::IndexedTriangle(mesh.indices[i], mesh.indices[i + 1], mesh.indices[i + 2]));
    }
    return makeCollider(JPH::MeshShapeSettings(verts, tris).Create(), "mesh");
}

Collider Collider::heightfield(const std::vector<float>& heights, uint32_t n, float cellSize, const glm::vec3& origin)
{
    requireJolt("heightfield");
    if (heights.size() < size_t(n) * n || n < 4) throw std::runtime_error("Collider::heightfield: not enough samples");
    // Jolt wants a sample count that's a multiple of its block size (2);
    // pad by repeating the last row/column if needed.
    const uint32_t padded = (n % 2 == 0) ? n : n + 1;
    std::vector<float> samples(size_t(padded) * padded);
    for (uint32_t z = 0; z < padded; ++z) {
        for (uint32_t x = 0; x < padded; ++x) {
            samples[size_t(z) * padded + x] = heights[size_t(std::min(z, n - 1)) * n + std::min(x, n - 1)];
        }
    }
    JPH::HeightFieldShapeSettings settings(samples.data(), toJ(origin), JPH::Vec3(cellSize, 1.0f, cellSize), padded);
    return makeCollider(settings.Create(), "heightfield");
}

Collider Collider::offset(const glm::vec3& translation, const glm::quat& rotation) const
{
    if (!impl) return *this;
    return makeCollider(JPH::RotatedTranslatedShapeSettings(toJ(translation), toJ(rotation), impl->shape).Create(), "offset");
}

uint32_t PhysicsWorld::createStaticBody(const Collider& collider, const glm::vec3& position, const glm::quat& rotation,
                                       const glm::vec3& scale, uint32_t userValue)
{
    if (!collider.valid()) return Rigidbody::kInvalidBody;
    JPH::BodyCreationSettings s(scaledShape(collider, scale), toJ(position), toJ(rotation), JPH::EMotionType::Static,
                                Layers::NonMoving);
    s.mUserData = staticUserData(userValue);
    const JPH::BodyID id = m_impl->system->GetBodyInterface().CreateAndAddBody(s, JPH::EActivation::DontActivate);
    if (id.IsInvalid()) return Rigidbody::kInvalidBody;
    ++m_impl->addedSinceOptimize;
    return id.GetIndexAndSequenceNumber();
}

void PhysicsWorld::destroyStaticBody(uint32_t bodyId)
{
    if (bodyId == Rigidbody::kInvalidBody || !m_impl->system) return;
    JPH::BodyInterface& bi = m_impl->system->GetBodyInterface();
    bi.RemoveBody(JPH::BodyID(bodyId));
    bi.DestroyBody(JPH::BodyID(bodyId));
}

// =============================================================================
// Rigidbody
// =============================================================================

Rigidbody::Rigidbody(Collider c, BodyType t) : collider(std::move(c)), type(t) {}

Rigidbody::~Rigidbody()
{
    destroyBody();
}

void Rigidbody::rebuild()
{
    if (!m_startedOnce) return; // start() will build it with the new settings
    destroyBody();
    if (shape != ColliderShape::Custom) collider = {};
    start();
}

void Rigidbody::start()
{
    m_startedOnce = true;
    if (!collider.valid() && shape != ColliderShape::Custom) {
        const glm::vec3 s = glm::max(shapeSize, glm::vec3(0.01f));
        switch (shape) {
        case ColliderShape::Box: collider = Collider::box(s * 0.5f); break;
        case ColliderShape::Sphere: collider = Collider::sphere(s.x * 0.5f); break;
        case ColliderShape::Capsule:
            collider = Collider::capsule(std::max(s.y * 0.5f - s.x * 0.5f, 0.01f), s.x * 0.5f);
            break;
        case ColliderShape::Cylinder: collider = Collider::cylinder(s.y * 0.5f, s.x * 0.5f); break;
        default: break;
        }
        if (shapeOffset != glm::vec3(0.0f)) collider = collider.offset(shapeOffset);
    }
    if (!collider.valid()) {
        log::warn("Rigidbody on '{}' has no collider", gameObject().name());
        return;
    }
    // Bake the object's world scale into the shape.
    const glm::mat4& m = transform().worldMatrix();
    const glm::vec3 scale(glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2])));
    JPH::RefConst<JPH::Shape> shape = scaledShape(collider, scale);

    if (shape->MustBeStatic() && type != BodyType::Static) {
        log::warn("Rigidbody on '{}': mesh/heightfield colliders must be static; making it static",
                  gameObject().name());
        type = BodyType::Static;
    }

    const glm::vec3 pos = transform().position();
    const glm::quat rot = transform().rotation();
    JPH::EMotionType motion = type == BodyType::Static      ? JPH::EMotionType::Static
                              : type == BodyType::Kinematic ? JPH::EMotionType::Kinematic
                                                            : JPH::EMotionType::Dynamic;
    // A static sensor only sees awake bodies; a kinematic one that never
    // sleeps (and may touch non-dynamic bodies) sees characters and sleepers too.
    const bool sensorAsKinematic = isTrigger && motion == JPH::EMotionType::Static && !shape->MustBeStatic();
    if (sensorAsKinematic) motion = JPH::EMotionType::Kinematic;
    if (isTrigger && shape->MustBeStatic())
        log::warn("Rigidbody on '{}': mesh/heightfield triggers only detect awake dynamic bodies; use a convex shape",
                  gameObject().name());
    JPH::BodyCreationSettings s(shape, toJ(pos), toJ(rot), motion,
                                isTrigger                    ? Layers::Sensor
                                : type == BodyType::Static ? Layers::NonMoving
                                                           : Layers::Moving);
    if (isTrigger) {
        s.mIsSensor = true;
        s.mCollideKinematicVsNonDynamic = true;
        s.mAllowSleeping = false;
    }
    s.mFriction = friction;
    s.mRestitution = restitution;
    s.mLinearDamping = linearDamping;
    s.mAngularDamping = angularDamping;
    s.mMotionQuality = continuousCollision ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
    s.mUserData = reinterpret_cast<JPH::uint64>(this); // lets raycasts find us
    if (type == BodyType::Dynamic && mass > 0.0f) {
        s.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        s.mMassPropertiesOverride.mMass = mass;
    }
    if (m_hasPendingVelocity) s.mLinearVelocity = toJ(m_pendingVelocity);

    auto& impl = physics().impl();
    const JPH::BodyID id = impl.system->GetBodyInterface().CreateAndAddBody(
        s, motion == JPH::EMotionType::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
    if (id.IsInvalid()) {
        log::error("Rigidbody on '{}': out of physics bodies", gameObject().name());
        return;
    }
    m_bodyId = id.GetIndexAndSequenceNumber();
    m_prevPos = m_currPos = pos;
    m_prevRot = m_currRot = rot;
    ++impl.addedSinceOptimize;
}

void Rigidbody::destroyBody()
{
    if (!created()) return;
    if (physicsAlive()) {
        JPH::BodyInterface& bi = bodies();
        bi.RemoveBody(JPH::BodyID(m_bodyId));
        bi.DestroyBody(JPH::BodyID(m_bodyId));
    }
    m_bodyId = kInvalidBody;
}

void Rigidbody::onDestroy()
{
    destroyBody();
}

void Rigidbody::addForce(const glm::vec3& f)
{
    if (created()) bodies().AddForce(JPH::BodyID(m_bodyId), toJ(f));
}

void Rigidbody::addImpulse(const glm::vec3& i)
{
    if (created()) bodies().AddImpulse(JPH::BodyID(m_bodyId), toJ(i));
}

void Rigidbody::addImpulseAt(const glm::vec3& i, const glm::vec3& p)
{
    if (created()) bodies().AddImpulse(JPH::BodyID(m_bodyId), toJ(i), toJ(p));
}

glm::vec3 Rigidbody::linearVelocity() const
{
    return created() ? toG(bodies().GetLinearVelocity(JPH::BodyID(m_bodyId))) : m_pendingVelocity;
}

void Rigidbody::setLinearVelocity(const glm::vec3& v)
{
    if (created()) {
        bodies().SetLinearVelocity(JPH::BodyID(m_bodyId), toJ(v));
    } else {
        // Not created yet (we're before start()): apply at creation.
        m_pendingVelocity = v;
        m_hasPendingVelocity = true;
    }
}

glm::vec3 Rigidbody::angularVelocity() const
{
    return created() ? toG(bodies().GetAngularVelocity(JPH::BodyID(m_bodyId))) : glm::vec3(0.0f);
}

void Rigidbody::setAngularVelocity(const glm::vec3& w)
{
    if (created()) bodies().SetAngularVelocity(JPH::BodyID(m_bodyId), toJ(w));
}

void Rigidbody::teleport(const glm::vec3& position, const glm::quat& rotation)
{
    transform().setPosition(position);
    transform().setRotation(rotation);
    m_prevPos = m_currPos = position;
    m_prevRot = m_currRot = rotation;
    if (created()) {
        bodies().SetPositionAndRotation(JPH::BodyID(m_bodyId), toJ(position), toJ(rotation),
                                        type == BodyType::Static ? JPH::EActivation::DontActivate
                                                                 : JPH::EActivation::Activate);
    }
}

void Rigidbody::syncFromTransform()
{
    teleport(transform().position(), transform().rotation());
}

bool Rigidbody::isAwake() const
{
    return created() && bodies().IsActive(JPH::BodyID(m_bodyId));
}

// =============================================================================
// CharacterController
// =============================================================================

struct CharacterController::Impl {
    JPH::Ref<JPH::CharacterVirtualSettings> settings;
    JPH::Ref<JPH::CharacterVirtual> character;
};

CharacterController::CharacterController() = default;

CharacterController::~CharacterController()
{
    destroy();
}

void CharacterController::destroy()
{
    // The character owns an inner body in the physics system; release it while the system exists.
    if (m_impl && !physicsAlive()) {
        // System already gone: leak-free because Jolt freed everything with the system.
        (void)m_impl.release();
        return;
    }
    m_impl.reset();
}

void CharacterController::onDestroy()
{
    destroy();
}

void CharacterController::start()
{
    auto& world = physics().impl();
    const float halfHeight = std::max(0.01f, height * 0.5f - radius);
    // Capsule standing on the origin, so the character's position is its feet.
    JPH::RefConst<JPH::Shape> capsule =
        JPH::RotatedTranslatedShapeSettings(JPH::Vec3(0.0f, halfHeight + radius, 0.0f), JPH::Quat::sIdentity(),
                                            new JPH::CapsuleShape(halfHeight, radius))
            .Create()
            .Get();

    m_impl = std::make_unique<Impl>();
    m_impl->settings = new JPH::CharacterVirtualSettings();
    auto& s = *m_impl->settings;
    s.mShape = capsule;
    s.mMaxSlopeAngle = glm::radians(maxSlopeDegrees);
    // Contacts below the centre of the bottom sphere count as "standing on something".
    s.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -radius);
    s.mMass = mass;
    s.mMaxStrength = pushStrength;
    // An inner rigid body so dynamic objects (thrown rocks...) can hit the character.
    s.mInnerBodyShape = capsule;
    s.mInnerBodyLayer = Layers::Character;

    const glm::vec3 feet = transform().position();
    // The inner body carries the same user data, so contacts and queries find us.
    m_impl->character = new JPH::CharacterVirtual(&s, toJ(feet), JPH::Quat::sIdentity(), characterUserData(this),
                                                  world.system.get());
    m_prevPos = m_currPos = feet;
}

void CharacterController::move(const glm::vec3& horizontalVelocity, float dt)
{
    if (!m_impl || dt <= 0.0f) return;
    auto& world = physics().impl();
    JPH::CharacterVirtual& c = *m_impl->character;

    // Velocity update, following Jolt's CharacterVirtual sample:
    // on the ground we move with the ground (moving platforms) and may jump;
    // in the air we keep our vertical speed. Gravity always applies.
    c.UpdateGroundVelocity();
    const JPH::Vec3 groundVelocity = c.GetGroundVelocity();
    const JPH::Vec3 current = c.GetLinearVelocity();
    const bool movingTowardsGround = (current.GetY() - groundVelocity.GetY()) < 0.1f;
    JPH::Vec3 v;
    if (c.GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround && movingTowardsGround) {
        v = groundVelocity;
        if (m_jumpSpeed > 0.0f) v += JPH::Vec3(0.0f, m_jumpSpeed, 0.0f);
    } else {
        v = JPH::Vec3(0.0f, current.GetY(), 0.0f);
    }
    m_jumpSpeed = 0.0f;
    v += world.system->GetGravity() * dt;
    v += JPH::Vec3(horizontalVelocity.x, 0.0f, horizontalVelocity.z);
    c.SetLinearVelocity(v);

    JPH::CharacterVirtual::ExtendedUpdateSettings update;
    update.mStickToFloorStepDown = JPH::Vec3(0.0f, -std::max(stepHeight, 0.3f), 0.0f);
    update.mWalkStairsStepUp = JPH::Vec3(0.0f, stepHeight, 0.0f);
    JPH::IgnoreSingleBodyFilter ignoreSelf(c.GetInnerBodyID());
    c.ExtendedUpdate(dt, world.system->GetGravity(), update,
                     world.system->GetDefaultBroadPhaseLayerFilter(Layers::Moving),
                     world.system->GetDefaultLayerFilter(Layers::Moving), ignoreSelf, {}, *world.temp);

    m_prevPos = m_currPos;
    m_currPos = toG(c.GetPosition());
}

uint32_t CharacterController::innerBodyId() const
{
    return m_impl ? m_impl->character->GetInnerBodyID().GetIndexAndSequenceNumber() : Rigidbody::kInvalidBody;
}

bool CharacterController::grounded() const
{
    return m_impl && m_impl->character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
}

bool CharacterController::onSteepGround() const
{
    return m_impl && m_impl->character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnSteepGround;
}

glm::vec3 CharacterController::velocity() const
{
    return m_impl ? toG(m_impl->character->GetLinearVelocity()) : glm::vec3(0.0f);
}

glm::vec3 CharacterController::groundNormal() const
{
    return m_impl ? toG(m_impl->character->GetGroundNormal()) : glm::vec3(0, 1, 0);
}

void CharacterController::teleport(const glm::vec3& feet)
{
    transform().setPosition(feet);
    m_prevPos = m_currPos = feet;
    if (m_impl) {
        m_impl->character->SetPosition(toJ(feet));
        m_impl->character->SetLinearVelocity(JPH::Vec3::sZero());
    }
}

} // namespace ve
