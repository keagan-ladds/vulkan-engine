#pragma once

// =============================================================================
// Player - first-person character built on ve::CharacterController.
//
// Demonstrates the update/fixedUpdate split:
//  - update():      mouse look, reading one-shot key presses (jump, fly, use, throw)
//  - fixedUpdate(): walking at a fixed 50 Hz through the CharacterController
//                   (gravity, slopes, steps, collisions, pushing crates)
//  - the controller interpolates the rendered position between physics steps,
//    so motion stays smooth even though physics runs at 50 Hz.
// =============================================================================

#include <ve/physics/Physics.hpp>
#include <ve/scene/GameObject.hpp>

#include <memory>
#include <vector>

namespace ve {
class Mesh;
class Material;
} // namespace ve

namespace savanna {

class Terrain;

class Player : public ve::GameObject {
public:
    explicit Player(const Terrain& terrain);

    void start() override;
    void update(float dt) override;
    void fixedUpdate(float dt) override;

    void teleport(const glm::vec3& feet, float yawRadians);
    // Debug: fly mode at a given position, yaw/pitch in degrees.
    void setPose(const glm::vec3& feet, float yawDegrees, float pitchDegrees);
    // What Q / right mouse throws.
    void setThrowable(std::shared_ptr<ve::Mesh> mesh, std::shared_ptr<ve::Material> material, ve::Collider collider);
    void throwRock();

    glm::vec3 eyePosition() const;
    glm::vec3 lookDirection() const;
    bool flying() const { return m_fly; }
    bool grounded() const;
    bool usePressed() const { return m_usePressed; }

    float walkSpeed = 4.5f;
    float sprintSpeed = 9.0f;
    float flySpeed = 25.0f;
    float jumpSpeed = 5.5f;
    float throwSpeed = 16.0f;
    float eyeHeight = 1.7f;
    float mouseSensitivity = 0.0022f;

private:
    const Terrain& m_terrain;
    ve::CharacterController* m_controller = nullptr;
    ve::GameObject* m_head = nullptr;
    glm::vec3 m_walkVelocity{0.0f};
    float m_yaw = 0.0f, m_pitch = 0.0f;
    bool m_jumpRequested = false;
    bool m_fly = false;
    bool m_usePressed = false;

    std::shared_ptr<ve::Mesh> m_rockMesh;
    std::shared_ptr<ve::Material> m_rockMaterial;
    ve::Collider m_rockCollider;
    std::vector<ve::GameObject*> m_thrown;
};

} // namespace savanna
