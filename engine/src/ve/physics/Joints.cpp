// =============================================================================
// Joints: Fixed / Hinge / Slider / Distance components on Jolt constraints.
// (Jolt-backed: see JoltInternal.hpp for the module's layout and its rule.
// Joint::drawDebug is in PhysicsDebug.cpp, with the rest of the debug drawing.)
// =============================================================================

#include "ve/physics/JoltInternal.hpp"

#include "ve/scene/Scene.hpp"

#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>

#include <cmath>

namespace ve {

using namespace physics_detail;

struct Joint::Impl {
    JPH::Ref<JPH::TwoBodyConstraint> constraint;
    uint32_t body1 = Rigidbody::kInvalidBody, body2 = Rigidbody::kInvalidBody;
};

Joint::Joint(Type t) : m_type(t) {}

Joint::~Joint()
{
    detach();
}

void Joint::connect(GameObject* other)
{
    detach();
    m_connected = other;
    connectedTo = other ? other->name() : std::string();
    m_findWait = 0;
}

bool Joint::attached() const
{
    return m_impl && m_impl->constraint;
}

void Joint::loadState(const Json& in)
{
    m_broken = in.value("broken", false);
    if (m_broken) detach();
}

void Joint::onDestroy()
{
    detach();
}

void Joint::detach()
{
    if (!m_impl || !m_impl->constraint) return;
    if (physicsAlive()) {
        auto& impl = Engine::get().physics().impl();
        impl.system->RemoveConstraint(m_impl->constraint);
        std::erase(impl.joints, this);
    }
    m_impl->constraint = nullptr;
}

void Joint::detachIfUsing(uint32_t body)
{
    if (m_impl && m_impl->constraint && (m_impl->body1 == body || m_impl->body2 == body)) detach();
}

void Joint::fixedUpdate(float)
{
    if (!m_broken && !attached()) attach();
    if (attached()) applyMotor();
}

GameObject* Joint::connectedObject() const
{
    return m_connected.get(gameObject().scene());
}

bool Joint::attach()
{
    if (!physicsAlive()) return false;
    Rigidbody* own = gameObject().getComponent<Rigidbody>();
    if (!own || !own->created()) return false;
    // The other body: by id (connect()), else by name (scene files); none = the world.
    Rigidbody* other = nullptr;
    GameObject* otherObject = connectedObject();
    if (!otherObject && !connectedTo.empty()) {
        // A search by name walks the whole scene: while the other isn't there
        // yet, look again only every few steps (fixedUpdate retries each one).
        if (m_findWait > 0) {
            --m_findWait;
            return false;
        }
        otherObject = gameObject().scene().find(connectedTo);
        if (!otherObject) m_findWait = 15;
    }
    if (!connectedTo.empty() || !m_connected.empty()) {
        if (!otherObject) return false; // (not there yet)
        other = otherObject->getComponent<Rigidbody>();
        if (!other || !other->created()) return false;
        m_connected = otherObject;
    }
    const Transform& t = transform();
    const glm::vec3 p1 = glm::vec3(t.worldMatrix() * glm::vec4(anchor, 1.0f));
    glm::vec3 ax = glm::length(axis) > 1e-6f ? glm::normalize(t.rotation() * axis) : glm::vec3(0, 1, 0);
    glm::vec3 normal = glm::cross(ax, std::abs(ax.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0));
    normal = glm::normalize(glm::cross(normal, ax)); // perpendicular to the axis
    auto spring = [&] {
        JPH::SpringSettings sp;
        sp.mFrequency = std::max(0.0f, springFrequency);
        sp.mDamping = springDamping;
        return sp;
    };
    JPH::Ref<JPH::TwoBodyConstraintSettings> settings;
    switch (m_type) {
    case Type::Fixed: {
        auto* f = new JPH::FixedConstraintSettings();
        f->mPoint1 = f->mPoint2 = toJ(p1);
        const glm::vec3 x = t.rotation() * glm::vec3(1, 0, 0), y = t.rotation() * glm::vec3(0, 1, 0);
        f->mAxisX1 = f->mAxisX2 = toJ(x);
        f->mAxisY1 = f->mAxisY2 = toJ(y);
        settings = f;
        break;
    }
    case Type::Hinge: {
        auto* h = new JPH::HingeConstraintSettings();
        h->mPoint1 = h->mPoint2 = toJ(p1);
        h->mHingeAxis1 = h->mHingeAxis2 = toJ(ax);
        h->mNormalAxis1 = h->mNormalAxis2 = toJ(normal);
        if (useLimits) {
            h->mLimitsMin = glm::radians(std::min(limitMin, 0.0f));
            h->mLimitsMax = glm::radians(std::max(limitMax, 0.0f));
            h->mLimitsSpringSettings = spring();
        }
        h->mMaxFrictionTorque = friction;
        h->mMotorSettings = JPH::MotorSettings(std::max(0.1f, motorFrequency), 1.0f);
        h->mMotorSettings.SetTorqueLimit(motorMaxForce);
        settings = h;
        break;
    }
    case Type::Slider: {
        auto* sl = new JPH::SliderConstraintSettings();
        sl->mPoint1 = sl->mPoint2 = toJ(p1);
        sl->mSliderAxis1 = sl->mSliderAxis2 = toJ(ax);
        sl->mNormalAxis1 = sl->mNormalAxis2 = toJ(normal);
        if (useLimits) {
            sl->mLimitsMin = std::min(limitMin, 0.0f);
            sl->mLimitsMax = std::max(limitMax, 0.0f);
            sl->mLimitsSpringSettings = spring();
        }
        sl->mMaxFrictionForce = friction;
        sl->mMotorSettings = JPH::MotorSettings(std::max(0.1f, motorFrequency), 1.0f);
        sl->mMotorSettings.SetForceLimit(motorMaxForce);
        settings = sl;
        break;
    }
    case Type::Distance: {
        auto* d = new JPH::DistanceConstraintSettings();
        const glm::vec3 p2 = other ? glm::vec3(otherObject->transform().worldMatrix() * glm::vec4(connectedAnchor, 1.0f)) : connectedAnchor;
        d->mPoint1 = toJ(p1);
        d->mPoint2 = toJ(p2);
        d->mMinDistance = minDistance;
        d->mMaxDistance = maxDistance;
        d->mLimitsSpringSettings = spring();
        settings = d;
        break;
    }
    }
    auto& impl = Engine::get().physics().impl();
    JPH::BodyInterface& bi = impl.system->GetBodyInterface();
    const JPH::BodyID id1(own->bodyId());
    const JPH::BodyID id2 = other ? JPH::BodyID(other->bodyId()) : JPH::BodyID();
    JPH::TwoBodyConstraint* c = bi.CreateConstraint(settings, id1, id2);
    if (!c) return false;
    if (!m_impl) m_impl = std::make_unique<Impl>();
    m_impl->constraint = c;
    m_impl->body1 = own->bodyId();
    m_impl->body2 = other ? other->bodyId() : Rigidbody::kInvalidBody;
    impl.system->AddConstraint(c);
    impl.joints.push_back(this);
    bi.ActivateConstraint(c);
    applyMotor();
    return true;
}

void Joint::applyMotor()
{
    JPH::TwoBodyConstraint* c = m_impl->constraint;
    const JPH::EMotorState state = motor == Motor::Velocity   ? JPH::EMotorState::Velocity
                                   : motor == Motor::Position ? JPH::EMotorState::Position
                                                              : JPH::EMotorState::Off;
    bool changed = false;
    if (m_type == Type::Hinge) {
        auto* h = static_cast<JPH::HingeConstraint*>(c);
        changed = h->GetMotorState() != state || (state == JPH::EMotorState::Velocity && h->GetTargetAngularVelocity() != glm::radians(motorTarget)) ||
                  (state == JPH::EMotorState::Position && h->GetTargetAngle() != glm::radians(motorTarget));
        h->SetMotorState(state);
        h->SetTargetAngularVelocity(glm::radians(motorTarget));
        h->SetTargetAngle(glm::radians(motorTarget));
        h->GetMotorSettings().SetTorqueLimit(motorMaxForce);
    } else if (m_type == Type::Slider) {
        auto* sl = static_cast<JPH::SliderConstraint*>(c);
        changed = sl->GetMotorState() != state || (state == JPH::EMotorState::Velocity && sl->GetTargetVelocity() != motorTarget) ||
                  (state == JPH::EMotorState::Position && sl->GetTargetPosition() != motorTarget);
        sl->SetMotorState(state);
        sl->SetTargetVelocity(motorTarget);
        sl->SetTargetPosition(motorTarget);
        sl->GetMotorSettings().SetForceLimit(motorMaxForce);
    }
    // A motor that wants something: keep the bodies awake.
    if (changed || state == JPH::EMotorState::Velocity) Engine::get().physics().impl().system->GetBodyInterface().ActivateConstraint(c);
}

void Joint::afterStep(float dt)
{
    if (!attached() || dt <= 0.0f) return;
    JPH::TwoBodyConstraint* c = m_impl->constraint;
    // Lambdas are the impulses the solver applied this step: / dt = forces.
    float lin = 0.0f, ang = 0.0f;
    switch (m_type) {
    case Type::Fixed: {
        auto* f = static_cast<JPH::FixedConstraint*>(c);
        lin = f->GetTotalLambdaPosition().Length();
        ang = f->GetTotalLambdaRotation().Length();
        break;
    }
    case Type::Hinge: {
        auto* h = static_cast<JPH::HingeConstraint*>(c);
        const JPH::Vector<2> r = h->GetTotalLambdaRotation();
        lin = h->GetTotalLambdaPosition().Length();
        ang = std::sqrt(r[0] * r[0] + r[1] * r[1]) + std::abs(h->GetTotalLambdaRotationLimits());
        break;
    }
    case Type::Slider: {
        auto* sl = static_cast<JPH::SliderConstraint*>(c);
        const JPH::Vector<2> p = sl->GetTotalLambdaPosition();
        lin = std::sqrt(p[0] * p[0] + p[1] * p[1]) + std::abs(sl->GetTotalLambdaPositionLimits());
        ang = sl->GetTotalLambdaRotation().Length();
        break;
    }
    case Type::Distance: lin = std::abs(static_cast<JPH::DistanceConstraint*>(c)->GetTotalLambdaPosition()); break;
    }
    m_force = lin / dt;
    m_torque = ang / dt;
    if ((breakForce > 0.0f && m_force > breakForce) || (breakTorque > 0.0f && m_torque > breakTorque)) {
        m_broken = true;
        detach();
        if (onBreak) onBreak(*this);
    }
}

float Joint::angle() const
{
    if (!attached() || m_type != Type::Hinge) return 0.0f;
    return glm::degrees(static_cast<const JPH::HingeConstraint*>(m_impl->constraint.GetPtr())->GetCurrentAngle());
}

float Joint::position() const
{
    if (!attached()) return 0.0f;
    if (m_type == Type::Slider) return static_cast<const JPH::SliderConstraint*>(m_impl->constraint.GetPtr())->GetCurrentPosition();
    if (m_type == Type::Distance) {
        const glm::vec3 a = glm::vec3(transform().worldMatrix() * glm::vec4(anchor, 1.0f));
        const GameObject* o = connectedObject();
        const glm::vec3 b = o ? glm::vec3(o->transform().worldMatrix() * glm::vec4(connectedAnchor, 1.0f)) : connectedAnchor;
        return glm::distance(a, b);
    }
    return 0.0f;
}

} // namespace ve
