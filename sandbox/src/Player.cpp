#include "Player.hpp"

#include "Building.hpp"

#include <ve/gameplay/Interaction.hpp>
#include <ve/world/Grass.hpp>
#include <ve/world/Terrain.hpp>

#include <ve/core/Engine.hpp>
#include <ve/core/Input.hpp>
#include <ve/core/Time.hpp>
#include <ve/scene/Components.hpp>
#include <ve/scene/Scene.hpp>

#include <algorithm>

namespace savanna {

using ve::Key;

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
    m_head = &scene().spawn("Head");
    m_head->setParent(this, false);
    m_head->transform().setLocalPosition({0.0f, eyeHeight, 0.0f});
    auto& cam = m_head->addComponent<ve::Camera>();
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
    out = {{"position", {p.x, p.y, p.z}}, {"yaw", m_yaw}, {"pitch", m_pitch}, {"flying", m_fly}};
}

void Player::loadState(const ve::Json& in)
{
    const ve::Json& p = in.value("position", ve::Json::array());
    if (p.is_array() && p.size() == 3) teleport({p[0].get<float>(), p[1].get<float>(), p[2].get<float>()}, in.value("yaw", m_yaw));
    m_pitch = in.value("pitch", m_pitch);
    m_fly = in.value("flying", m_fly);
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
    return m_head ? m_head->transform().position() : transform().position() + glm::vec3(0, eyeHeight, 0);
}

glm::vec3 Player::lookDirection() const
{
    return m_head ? m_head->transform().forward() : transform().forward();
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
    body.mass = 3.0f;
    body.restitution = 0.25f;
    body.friction = 0.8f;
    body.continuousCollision = true; // fast + small: don't tunnel through things
    body.setLinearVelocity(dir * throwSpeed + m_controller->velocity());

    // Keep the world tidy: only the most recent rocks stay.
    m_thrown.push_back(&rock);
    if (m_thrown.size() > 30) {
        m_thrown.front()->destroy();
        m_thrown.erase(m_thrown.begin());
    }
}

void Player::update(float)
{
    ve::Input& in = input();
    if (in.keyPressed(Key::Tab)) in.setMouseCaptured(!in.mouseCaptured());
    if (in.keyPressed(Key::Escape)) engine().quit();

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
    if (m_head) m_head->transform().setLocalRotation(glm::angleAxis(m_pitch, glm::vec3(1, 0, 0)));

    // Edges ("pressed this frame") must be read in update(): fixedUpdate may
    // run zero or several times per frame. Hand them over as flags.
    if (in.actionPressed("Jump")) m_jumpRequested = true;
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
    float speed = in.actionDown("Sprint") ? sprintSpeed : walkSpeed;
    if (wading) speed *= 0.5f;

    // Smooth the horizontal velocity: snappy on the ground, floaty in the air.
    const float accel = m_controller->grounded() ? 12.0f : 2.0f;
    m_walkVelocity += (wish * speed - m_walkVelocity) * std::min(1.0f, accel * time().fixedDeltaTime);

    if (m_jumpRequested && m_controller->grounded()) m_controller->jump(jumpSpeed);
    m_jumpRequested = false;
    // The controller applies gravity, collides, climbs steps and pushes crates.
    m_controller->move(m_walkVelocity, time().fixedDeltaTime);
}

} // namespace savanna
