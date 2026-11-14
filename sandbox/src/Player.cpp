#include "Player.hpp"

#include "Terrain.hpp"

#include <ve/core/Engine.hpp>
#include <ve/core/Input.hpp>
#include <ve/core/Time.hpp>
#include <ve/scene/Components.hpp>
#include <ve/scene/Scene.hpp>

#include <algorithm>

namespace savanna {

using ve::Key;

Player::Player(const Terrain& terrain) : GameObject("Player"), m_terrain(terrain)
{
    // Input actions: gameplay code asks for "Jump" rather than a specific key,
    // so bindings can change without touching the logic.
    ve::Input& in = input();
    in.bindAction("Jump", Key::Space);
    in.bindAction("Sprint", Key::LShift);
    in.bindAction("Use", Key::E);
    in.bindAction("Use", ve::MouseButton::Left);
    in.bindAction("ToggleFly", Key::F);
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

void Player::teleport(const glm::vec3& feet, float yaw)
{
    m_position = m_prevPosition = feet;
    m_velocity = glm::vec3(0.0f);
    m_yaw = yaw;
    transform().setPosition(feet);
}

void Player::setPose(const glm::vec3& feet, float yawDegrees, float pitchDegrees)
{
    teleport(feet, glm::radians(yawDegrees));
    m_pitch = glm::radians(pitchDegrees);
    m_fly = true;
}

glm::vec3 Player::eyePosition() const
{
    return m_head ? m_head->transform().position() : transform().position() + glm::vec3(0, eyeHeight, 0);
}

glm::vec3 Player::lookDirection() const
{
    return m_head ? m_head->transform().forward() : transform().forward();
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
    transform().setLocalRotation(glm::angleAxis(m_yaw, glm::vec3(0, 1, 0)));
    if (m_head) m_head->transform().setLocalRotation(glm::angleAxis(m_pitch, glm::vec3(1, 0, 0)));

    // Edges ("pressed this frame") must be read in update(): fixedUpdate may
    // run zero or several times per frame. Hand them over as flags.
    if (in.actionPressed("Jump")) m_jumpRequested = true;
    if (in.actionPressed("ToggleFly")) {
        m_fly = !m_fly;
        m_velocity = glm::vec3(0.0f);
    }
    m_usePressed = in.actionPressed("Use");

    if (m_fly) {
        // Free flight is driven directly per frame (no physics).
        const float dt = time().unscaledDeltaTime;
        glm::vec3 move = lookDirection() * in.axis(Key::S, Key::W) + transform().right() * in.axis(Key::A, Key::D);
        move.y += in.axis(Key::LCtrl, Key::Space);
        const float speed = flySpeed * (in.actionDown("Sprint") ? 4.0f : 1.0f);
        m_position += move * speed * dt;
        const float ground = m_terrain.height(m_position.x, m_position.z);
        m_position.y = std::max(m_position.y, ground);
        m_prevPosition = m_position;
    }
}

void Player::fixedUpdate(float dt)
{
    if (m_fly) return;
    ve::Input& in = input();
    m_prevPosition = m_position;

    // Desired horizontal velocity from WASD, relative to where we face.
    const glm::vec3 fwd = glm::vec3(-std::sin(m_yaw), 0.0f, -std::cos(m_yaw));
    const glm::vec3 right = glm::vec3(std::cos(m_yaw), 0.0f, -std::sin(m_yaw));
    glm::vec3 wish = fwd * in.axis(Key::S, Key::W) + right * in.axis(Key::A, Key::D);
    if (glm::length(wish) > 1.0f) wish = glm::normalize(wish);
    const float groundH = m_terrain.height(m_position.x, m_position.z);
    const bool wading = groundH < Terrain::kWaterLevel - 0.3f;
    float speed = in.actionDown("Sprint") ? sprintSpeed : walkSpeed;
    if (wading) speed *= 0.5f;

    // Accelerate towards the wish velocity (snappy on ground, floaty in air).
    const glm::vec3 target = wish * speed;
    const float accel = m_grounded ? 12.0f : 2.0f;
    m_velocity.x += (target.x - m_velocity.x) * std::min(1.0f, accel * dt);
    m_velocity.z += (target.z - m_velocity.z) * std::min(1.0f, accel * dt);

    if (m_grounded && m_jumpRequested) {
        m_velocity.y = jumpSpeed;
        m_grounded = false;
    }
    m_jumpRequested = false;
    m_velocity.y -= gravity * dt;

    glm::vec3 next = m_position + m_velocity * dt;
    // Stay inside the world and off cliffs that are too steep to climb.
    next.x = std::clamp(next.x, -Terrain::kHalfSize + 30.0f, Terrain::kHalfSize - 30.0f);
    next.z = std::clamp(next.z, -Terrain::kHalfSize + 30.0f, Terrain::kHalfSize - 30.0f);
    const float nextGround = m_terrain.height(next.x, next.z);
    if (m_grounded && nextGround - groundH > 0.8f * std::max(0.05f, glm::length(glm::vec2(next.x - m_position.x, next.z - m_position.z))) + 0.25f) {
        next.x = m_position.x; // too steep: block horizontal movement
        next.z = m_position.z;
    }
    const float ground = m_terrain.height(next.x, next.z);
    if (next.y <= ground) {
        next.y = ground;
        m_velocity.y = 0.0f;
        m_grounded = true;
    } else if (m_grounded && next.y - ground < 0.3f && m_velocity.y <= 0.0f) {
        next.y = ground; // stick to the ground when walking downhill
        m_velocity.y = 0.0f;
    } else {
        m_grounded = false;
    }
    m_position = next;
}

void Player::lateUpdate(float)
{
    // Interpolate between the last two physics states for smooth rendering.
    const float alpha = m_fly ? 1.0f : time().fixedAlpha;
    transform().setPosition(glm::mix(m_prevPosition, m_position, alpha));
}

} // namespace savanna
