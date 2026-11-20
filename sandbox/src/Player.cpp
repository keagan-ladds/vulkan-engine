#include "Player.hpp"

#include "Building.hpp"
#include "Footsteps.hpp"
#include "Props.hpp"
#include "SavannaLayout.hpp"
#include "SceneLookup.hpp"

#include <ve/gameplay/Interaction.hpp>
#include <ve/world/Grass.hpp>
#include <ve/world/Terrain.hpp>

#include <ve/assets/AssetManager.hpp>
#include <ve/audio/Audio.hpp>
#include <ve/core/Engine.hpp>
#include <ve/core/Input.hpp>
#include <ve/core/Time.hpp>
#include <ve/scene/Components.hpp>
#include <ve/scene/Scene.hpp>

#include <algorithm>
#include <random>

namespace savanna {

using ve::Key;

Player* localPlayer(const ve::Scene& scene)
{
    static ve::ObjectRef<Player> s_player; // (validated on every call: a new level has a new player)
    return findCached(scene, s_player);
}

Player::Player() : GameObject("Player")
{
    // Input actions: gameplay code asks for "Jump" rather than a specific key,
    // so bindings can change without touching the logic.
    ve::Input& in = input();
    in.bindAction("Jump", Key::Space);
    in.bindAction("Sprint", Key::LShift);
    in.bindAction("Use", Key::E);
    in.bindAction("Use", ve::MouseButton::Left);
    in.bindAction("Throw", Key::Q);
    in.bindAction("Throw", ve::MouseButton::Right);
    in.bindAction("ToggleFly", Key::F);
    // Gamepad (Xbox layout): left stick moves, right stick looks.
    in.bindAction("Jump", ve::GamepadButton::A);
    in.bindAction("Use", ve::GamepadButton::X);
    in.bindAction("Throw", ve::GamepadAxis::RightTrigger);
    in.bindAction("Sprint", ve::GamepadButton::LeftStick);
    in.bindAction("ToggleFly", ve::GamepadButton::Y);
    in.bindAxis("MoveX", Key::A, Key::D);
    in.bindAxis("MoveX", ve::GamepadAxis::LeftX);
    in.bindAxis("MoveY", Key::S, Key::W);
    in.bindAxis("MoveY", ve::GamepadAxis::LeftY, -1.0f); // stick up = forward
    in.bindAxis("LookX", ve::GamepadAxis::RightX);
    in.bindAxis("LookY", ve::GamepadAxis::RightY);

    // Physics capsule that walks over terrain, rocks and steps, collides with
    // trees and huts, and pushes crates. Its position is our feet.
    m_controller = &addComponent<ve::CharacterController>();
    m_controller->radius = 0.35f;
    m_controller->height = 1.8f;
    m_controller->maxSlopeDegrees = 48.0f;
    m_controller->stepHeight = 0.45f;
    addComponent<ve::GrassBender>().radius = 0.7f; // walking through grass parts it
    addComponent<ve::Interactor>().action = "Use";   // looks for ve::Interactables (trees, bushes, sunstones)
}

void Player::start()
{
    // The camera lives on a child "Head" object at eye height: the body only
    // yaws, the head also pitches.
    ve::GameObject& head = scene().spawn("Head");
    m_head = &head;
    head.setParent(this, false);
    head.transform().setLocalPosition({0.0f, eyeHeight, 0.0f});
    auto& cam = head.addComponent<ve::Camera>();
    cam.fovDegrees = 65.0f;
    cam.nearPlane = 0.1f;
    cam.farPlane = 4000.0f;
    input().setMouseCaptured(true);
}

void Player::setThrowable(std::shared_ptr<ve::Mesh> mesh, std::shared_ptr<ve::Material> material, ve::Collider collider)
{
    m_rockMesh = std::move(mesh);
    m_rockMaterial = std::move(material);
    m_rockCollider = std::move(collider);
}

void Player::teleport(const glm::vec3& feet, float yaw)
{
    m_yaw = yaw;
    transform().setPosition(feet);
    m_controller->teleport(feet);
}

void Player::saveState(ve::Json& out) const
{
    const glm::vec3 p = transform().position();
    out = {{"position", {p.x, p.y, p.z}}, {"yaw", m_yaw}, {"pitch", m_pitch}, {"flying", m_fly}, {"health", health}, {"stamina", stamina}};
}

void Player::loadState(const ve::Json& in)
{
    const ve::Json& p = in.value("position", ve::Json::array());
    if (p.is_array() && p.size() == 3) teleport({p[0].get<float>(), p[1].get<float>(), p[2].get<float>()}, in.value("yaw", m_yaw));
    m_pitch = in.value("pitch", m_pitch);
    m_fly = in.value("flying", m_fly);
    health = in.value("health", maxHealth);
    stamina = in.value("stamina", maxStamina);
}

void Player::damage(float amount, const std::string& cause)
{
    if (amount <= 0.0f || m_fly) return;
    health = std::max(0.0f, health - amount);
    m_sinceHurt = 0.0f;
    if (health > 0.0f) return;
    // Collapsed: wake up at the camp, a little worse for wear.
    notify("You collapsed (" + cause + ") and woke up at the camp", 4.0f);
    health = maxHealth * 0.5f;
    stamina = maxStamina;
    const glm::vec2 c = layout::kCamp + glm::vec2(6.0f, 6.0f);
    const ve::Terrain* t = ve::Terrain::find(scene());
    teleport({c.x, t ? t->height(c.x, c.y) : 3.2f, c.y}, m_yaw);
}

void Player::setPose(const glm::vec3& feet, float yawDegrees, float pitchDegrees)
{
    teleport(feet, glm::radians(yawDegrees));
    m_pitch = glm::radians(pitchDegrees);
    m_fly = true;
}

bool Player::grounded() const
{
    return m_controller->grounded();
}

glm::vec3 Player::eyePosition() const
{
    const ve::GameObject* head = m_head.get(scene());
    return head ? head->transform().position() : transform().position() + glm::vec3(0, eyeHeight, 0);
}

glm::vec3 Player::lookDirection() const
{
    const ve::GameObject* head = m_head.get(scene());
    return head ? head->transform().forward() : transform().forward();
}

void Player::throwRock()
{
    if (!m_rockMesh || !m_rockCollider.valid()) return;
    // A small dynamic body launched along the view direction. The renderer
    // and physics both pick it up automatically from its components.
    auto& rock = scene().spawn("Thrown rock");
    rock.tag = "thrown";
    const glm::vec3 dir = lookDirection();
    rock.transform().setPosition(eyePosition() + dir * 0.7f);
    rock.transform().setLocalScale(0.22f);
    rock.addComponent<ve::MeshRenderer>(m_rockMesh, m_rockMaterial);
    auto& body = rock.addComponent<ve::Rigidbody>(m_rockCollider, ve::BodyType::Dynamic);
    body.layer = "Debris";  // (Savanna's collision layer for loose things: the Use ray looks past them)
    body.surface = "Stone";
    body.mass = 3.0f;
    body.restitution = 0.25f;
    body.friction = 0.8f;
    body.continuousCollision = true; // fast + small: don't tunnel through things
    body.setLinearVelocity(dir * throwSpeed + m_controller->velocity());

    // Keep the world tidy: only the most recent rocks stay.
    m_thrown.push_back(&rock);
    if (m_thrown.size() > 30) {
        if (ve::GameObject* oldest = m_thrown.front().get(scene())) oldest->destroy();
        m_thrown.pop_front();
    }
}

void Player::update(float)
{
    ve::Input& in = input();
    // (Escape opens the pause menu and Tab the inventory: MainMenu.hpp, Inventory.hpp.)

    // Look (always in real time, unaffected by Time::timeScale).
    if (in.mouseCaptured()) {
        const glm::vec2 d = in.mouseDelta() * mouseSensitivity;
        m_yaw -= d.x;
        m_pitch = std::clamp(m_pitch - d.y, -1.5f, 1.5f);
    }
    // Gamepad look: radians per second at full deflection.
    const float stick = 2.6f * time().unscaledDeltaTime;
    m_yaw -= in.axisValue("LookX") * stick;
    m_pitch = std::clamp(m_pitch - in.axisValue("LookY") * stick, -1.5f, 1.5f);
    transform().setLocalRotation(glm::angleAxis(m_yaw, glm::vec3(0, 1, 0)));
    if (ve::GameObject* head = m_head.get(scene())) head->transform().setLocalRotation(glm::angleAxis(m_pitch, glm::vec3(1, 0, 0)));

    // Edges ("pressed this frame") must be read in update(): fixedUpdate may
    // run zero or several times per frame. Hand them over as flags.
    if (in.actionPressed("Jump") && (m_fly || stamina >= 10.0f)) m_jumpRequested = true;

    // Vitals: stamina comes back a moment after it was last used, health
    // slowly after a while without getting hurt. Standing in a fire burns.
    const float dt = time().deltaTime;
    m_sinceStamina += dt;
    m_sinceHurt += dt;
    if (m_sinceStamina > 0.8f) stamina = std::min(maxStamina, stamina + 18.0f * dt);
    if (m_exhausted && stamina > maxStamina * 0.3f) m_exhausted = false;
    if (m_sinceHurt > 4.0f) health = std::min(maxHealth, health + 1.5f * dt);
    if (!m_fly)
        for (const auto& campfire : Campfire::all())
            if (const Campfire* fire = campfire.get(scene())) {
                const glm::vec3 d = transform().position() - fire->transform().position();
                if (glm::length(glm::vec2(d.x, d.z)) < 0.95f && std::abs(d.y) < 1.2f) damage(22.0f * dt, "burnt");
            }
    if (in.actionPressed("ToggleFly")) {
        m_fly = !m_fly;
        if (!m_fly) m_controller->teleport(transform().position()); // land where we are
    }

    if (in.actionPressed("Throw") && !Builder::building(scene())) throwRock(); // RMB removes pieces while building

    if (m_fly) {
        // Free flight (noclip): move directly, dragging the physics capsule along.
        const float dt = time().unscaledDeltaTime;
        glm::vec3 move = lookDirection() * in.axisValue("MoveY") + transform().right() * in.axisValue("MoveX");
        move.y += in.axis(Key::LCtrl, Key::Space);
        const float speed = flySpeed * (in.actionDown("Sprint") ? 4.0f : 1.0f);
        glm::vec3 p = transform().position() + move * speed * dt;
        if (const ve::Terrain* t = ve::Terrain::find(scene())) p.y = std::max(p.y, t->height(p.x, p.z));
        m_controller->teleport(p);
    }
}

void Player::fixedUpdate(float)
{
    if (m_fly) return;
    ve::Input& in = input();

    // Desired horizontal velocity from WASD, relative to where we face.
    const glm::vec3 fwd = glm::vec3(-std::sin(m_yaw), 0.0f, -std::cos(m_yaw));
    const glm::vec3 right = glm::vec3(std::cos(m_yaw), 0.0f, -std::sin(m_yaw));
    glm::vec3 wish = fwd * in.axisValue("MoveY") + right * in.axisValue("MoveX");
    if (glm::length(wish) > 1.0f) wish = glm::normalize(wish);
    const glm::vec3 feet = transform().position();
    const ve::Terrain* terrain = ve::Terrain::find(scene());
    const bool wading = terrain && terrain->height(feet.x, feet.z) < terrain->waterLevel - 0.3f;
    const float dt = time().fixedDeltaTime;
    m_sprinting = in.actionDown("Sprint") && !m_exhausted && glm::length(wish) > 0.1f && m_controller->grounded();
    if (m_sprinting) {
        stamina = std::max(0.0f, stamina - 20.0f * dt);
        m_sinceStamina = 0.0f;
        if (stamina <= 0.0f) m_exhausted = true;
    }
    float speed = m_sprinting ? sprintSpeed : walkSpeed;
    if (wading) speed *= 0.5f;

    // Smooth the horizontal velocity: snappy on the ground, floaty in the air.
    const float accel = m_controller->grounded() ? 12.0f : 2.0f;
    m_walkVelocity += (wish * speed - m_walkVelocity) * std::min(1.0f, accel * time().fixedDeltaTime);

    if (m_jumpRequested && m_controller->grounded()) {
        m_controller->jump(jumpSpeed);
        stamina = std::max(0.0f, stamina - 10.0f);
        m_sinceStamina = 0.0f;
    }
    m_jumpRequested = false;
    // Falls: the faster the landing, the more it hurts.
    const bool grounded = m_controller->grounded();
    if (!grounded) m_fallSpeed = std::max(m_fallSpeed, -m_controller->velocity().y);
    else if (!m_wasGrounded) {
        if (m_fallSpeed > 11.0f) damage((m_fallSpeed - 11.0f) * 9.0f, "a bad fall");
        m_fallSpeed = 0.0f;
    }
    if (grounded && !m_wasGrounded) footstep(); // landing
    m_wasGrounded = grounded;
    // The controller applies gravity, collides, climbs steps and pushes crates.
    m_controller->move(m_walkVelocity, time().fixedDeltaTime);
    // Footsteps every stride, by surface.
    const float v = glm::length(glm::vec2(m_walkVelocity.x, m_walkVelocity.z));
    if (grounded && v > 0.6f) {
        m_stride += v * dt;
        if (m_stride > (m_sprinting ? 2.3f : 1.5f)) {
            m_stride = 0.0f;
            footstep();
        }
    }
}

void Player::footstep()
{
    // The physics surface underfoot -> a sound (Footsteps.hpp).
    m_stepSurface = m_controller->groundSurface();
    const glm::vec3 feet = transform().position();
    const std::string sound = footstepSound(scene(), m_stepSurface, feet);
    m_stepSound = sound;
    static std::mt19937 rng(5);
    const int variant = int(rng() % 3);
    try {
        auto clip = assets().loadAudio("audio/footstep_" + sound + "_" + std::to_string(variant) + ".wav");
        ve::OneShotSettings s;
        s.volume = m_sprinting ? 0.45f : 0.3f;
        s.pitch = std::uniform_real_distribution<float>(0.9f, 1.1f)(rng);
        s.position = feet;
        s.maxDistance = 25.0f;
        audio().playOneShot(*clip, s);
    } catch (const std::exception&) {
    }
}

} // namespace savanna

namespace savanna {
ve::Camera* Player::camera() const
{
    const ve::GameObject* head = m_head.get(scene());
    return head ? head->getComponent<ve::Camera>() : nullptr;
}
} // namespace savanna
