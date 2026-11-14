#pragma once

// =============================================================================
// Player - first-person character controller.
//
// Demonstrates the update/fixedUpdate split:
//  - update():      mouse look, reading one-shot key presses (jump, fly, use)
//  - fixedUpdate(): movement physics at a fixed 50 Hz (gravity, jumping,
//                   ground following), so it behaves the same at any frame rate
//  - the rendered position is interpolated between fixed steps (Time::fixedAlpha)
//    so motion stays smooth even though physics runs at 50 Hz.
// =============================================================================

#include <ve/scene/GameObject.hpp>

namespace savanna {

class Terrain;

class Player : public ve::GameObject {
public:
    explicit Player(const Terrain& terrain);

    void start() override;
    void update(float dt) override;
    void fixedUpdate(float dt) override;
    void lateUpdate(float dt) override;

    void teleport(const glm::vec3& feet, float yawRadians);
    // Debug: fly mode at a given position, yaw/pitch in degrees.
    void setPose(const glm::vec3& feet, float yawDegrees, float pitchDegrees);
    glm::vec3 eyePosition() const;
    glm::vec3 lookDirection() const;
    bool flying() const { return m_fly; }
    bool usePressed() const { return m_usePressed; }

    float walkSpeed = 4.5f;
    float sprintSpeed = 9.0f;
    float flySpeed = 25.0f;
    float jumpSpeed = 5.5f;
    float gravity = 18.0f;
    float eyeHeight = 1.7f;
    float mouseSensitivity = 0.0022f;

private:
    const Terrain& m_terrain;
    ve::GameObject* m_head = nullptr;
    glm::vec3 m_position{0.0f};     // physics position (feet), advanced in fixedUpdate
    glm::vec3 m_prevPosition{0.0f};
    glm::vec3 m_velocity{0.0f};
    float m_yaw = 0.0f, m_pitch = 0.0f;
    bool m_grounded = false;
    bool m_jumpRequested = false;
    bool m_fly = false;
    bool m_usePressed = false;
};

} // namespace savanna
