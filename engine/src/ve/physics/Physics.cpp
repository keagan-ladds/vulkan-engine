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
#include <thread>

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
        // Static things only need to test against moving things.
        if (layer == Layers::NonMoving || layer == Layers::Character) return bp == BPLayers::Moving;
        return true;
    }
};

class ObjectPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override
    {
        if (a == Layers::NonMoving) return b == Layers::Moving;
        if (a == Layers::Moving) return true;
        return b == Layers::Moving; // character capsule: only hit by moving bodies
    }
};

// Ray filter that optionally ignores character capsules.
class RayLayerFilter final : public JPH::ObjectLayerFilter {
public:
    explicit RayLayerFilter(bool characters) : m_characters(characters) {}
    bool ShouldCollide(JPH::ObjectLayer layer) const override { return m_characters || layer != Layers::Character; }

private:
    bool m_characters;
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

// =============================================================================
// PhysicsWorld
// =============================================================================

struct PhysicsWorld::Impl {
    std::unique_ptr<JPH::TempAllocatorImpl> temp;
    std::unique_ptr<JPH::JobSystemThreadPool> jobs;
    BroadPhaseLayerMap broadPhaseLayers;
    ObjectVsBroadPhaseFilter objectVsBroadPhase;
    ObjectPairFilter objectPairs;
    std::unique_ptr<JPH::PhysicsSystem> system;
    uint32_t addedSinceOptimize = 0;
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

// Body user data: a Rigidbody* (always even, it's aligned), or (value << 1) | 1
// for GameObject-less static bodies.
JPH::uint64 staticUserData(uint32_t value)
{
    return (JPH::uint64(value) << 1) | 1u;
}

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
}

void PhysicsWorld::interpolate(Scene& scene, float alpha)
{
    scene.forEach<Rigidbody>([&](Rigidbody& rb) {
        if (!rb.created() || rb.type != BodyType::Dynamic) return;
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
    if (!m_impl->system || maxDistance <= 0.0f) return std::nullopt;
    const glm::vec3 dir = glm::normalize(direction);
    const JPH::RRayCast ray{toJ(origin), toJ(dir * maxDistance)};
    JPH::RayCastResult hit;
    RayLayerFilter layerFilter(hitCharacters);
    JPH::BodyFilter anyBody;
    JPH::IgnoreSingleBodyFilter ignoreBody(ignore && ignore->created() ? JPH::BodyID(ignore->bodyId()) : JPH::BodyID());
    const JPH::BodyFilter& bodyFilter = (ignore && ignore->created()) ? static_cast<const JPH::BodyFilter&>(ignoreBody) : anyBody;
    if (!m_impl->system->GetNarrowPhaseQuery().CastRay(ray, hit, {}, layerFilter, bodyFilter)) return std::nullopt;

    RaycastHit result;
    result.distance = hit.mFraction * maxDistance;
    result.point = origin + dir * result.distance;
    JPH::BodyLockRead lock(m_impl->system->GetBodyLockInterface(), hit.mBodyID);
    if (lock.Succeeded()) {
        const JPH::Body& body = lock.GetBody();
        result.normal = toG(body.GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, toJ(result.point)));
        const JPH::uint64 ud = body.GetUserData();
        if (ud & 1u) {
            result.userValue = static_cast<uint32_t>(ud >> 1);
        } else {
            result.body = reinterpret_cast<Rigidbody*>(ud);
            if (result.body) result.gameObject = &result.body->gameObject();
        }
    }
    return result;
}

// =============================================================================
// Collider
// =============================================================================

struct Collider::Impl {
    JPH::RefConst<JPH::Shape> shape;
};

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

void Rigidbody::start()
{
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
    const JPH::EMotionType motion = type == BodyType::Static      ? JPH::EMotionType::Static
                                    : type == BodyType::Kinematic ? JPH::EMotionType::Kinematic
                                                                  : JPH::EMotionType::Dynamic;
    JPH::BodyCreationSettings s(shape, toJ(pos), toJ(rot), motion,
                                type == BodyType::Static ? Layers::NonMoving : Layers::Moving);
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
        s, type == BodyType::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
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
    m_impl->character = new JPH::CharacterVirtual(&s, toJ(feet), JPH::Quat::sIdentity(), 0, world.system.get());
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
