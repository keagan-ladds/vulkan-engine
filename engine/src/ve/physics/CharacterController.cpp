// =============================================================================
// CharacterController: a walking capsule on Jolt's CharacterVirtual.
// (Jolt-backed: see JoltInternal.hpp for the module's layout and its rule.)
// =============================================================================

#include "ve/physics/JoltInternal.hpp"

#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>

namespace ve {

using namespace physics_detail;

struct CharacterController::Impl {
    JPH::Ref<JPH::CharacterVirtualSettings> settings;
    JPH::Ref<JPH::CharacterVirtual> character;
    int layer = 0; // collision layer
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
        // System already gone (shutdown): destroying the CharacterVirtual would
        // remove its inner body from the freed system. Deliberately leak the
        // small Impl instead (once, at exit) - Jolt's own memory went with the system.
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
    m_impl->layer = resolveLayer(layer, gameObject().name());
    s.mInnerBodyLayer = makeLayer(Layers::Character, m_impl->layer);

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
    const JPH::ObjectLayer as = makeLayer(Layers::Moving, m_impl->layer); // walks into what its layer collides with
    c.ExtendedUpdate(dt, world.system->GetGravity(), update, world.system->GetDefaultBroadPhaseLayerFilter(as),
                     world.system->GetDefaultLayerFilter(as), ignoreSelf, {}, *world.temp);

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

namespace {
template <class F>
auto withGround(const CharacterController::Impl* impl, F&& f) -> decltype(f(std::declval<const JPH::Body&>()))
{
    if (!impl || !physicsAlive() || impl->character->GetGroundState() == JPH::CharacterVirtual::EGroundState::InAir) return {};
    const JPH::BodyID id = impl->character->GetGroundBodyID();
    if (id.IsInvalid()) return {};
    JPH::BodyLockRead lock(Engine::get().physics().impl().system->GetBodyLockInterface(), id);
    if (!lock.Succeeded()) return {};
    return f(lock.GetBody());
}
} // namespace

std::string CharacterController::groundSurface() const
{
    return withGround(m_impl.get(), [](const JPH::Body& b) { return surfaceOf(b); });
}

GameObject* CharacterController::groundObject() const
{
    return withGround(m_impl.get(), [](const JPH::Body& b) { return ownerOf(b.GetUserData()).gameObject; });
}

uint32_t CharacterController::groundUserValue() const
{
    return withGround(m_impl.get(), [](const JPH::Body& b) { return ownerOf(b.GetUserData()).userValue; });
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
