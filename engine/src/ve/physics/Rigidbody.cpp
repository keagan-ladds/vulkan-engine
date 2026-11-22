// =============================================================================
// Rigidbody: a GameObject's static / kinematic / dynamic Jolt body.
// (Jolt-backed: see JoltInternal.hpp for the module's layout and its rule.)
// =============================================================================

#include "ve/physics/JoltInternal.hpp"

#include "ve/core/Log.hpp"

#include <Jolt/Physics/Body/BodyCreationSettings.h>

namespace ve {

using namespace physics_detail;

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
    JPH::RefConst<JPH::Shape> scaled = scaledShape(collider, scale);

    if (scaled->MustBeStatic() && type != BodyType::Static) {
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
    const bool sensorAsKinematic = isTrigger && motion == JPH::EMotionType::Static && !scaled->MustBeStatic();
    if (sensorAsKinematic) motion = JPH::EMotionType::Kinematic;
    if (isTrigger && scaled->MustBeStatic())
        log::warn("Rigidbody on '{}': mesh/heightfield triggers only detect awake dynamic bodies; use a convex shape",
                  gameObject().name());
    JPH::BodyCreationSettings s(scaled, toJ(pos), toJ(rot), motion,
                                makeLayer(isTrigger                    ? Layers::Sensor
                                          : type == BodyType::Static ? Layers::NonMoving
                                                                     : Layers::Moving,
                                          resolveLayer(layer, gameObject().name())));
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
    // Applied: a later rebuild() must not relaunch the body with it.
    m_hasPendingVelocity = false;
    m_pendingVelocity = glm::vec3(0.0f);
    m_prevPos = m_currPos = pos;
    m_prevRot = m_currRot = rot;
    ++impl.addedSinceOptimize;
}

void Rigidbody::destroyBody()
{
    if (!created()) return;
    if (physicsAlive()) {
        // Joints hold the body: they come off first (and attach again if it's rebuilt).
        auto joints = Engine::get().physics().impl().joints;
        for (Joint* j : joints) j->detachIfUsing(m_bodyId);
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

} // namespace ve
