#pragma once

// =============================================================================
// Physics (backed by Jolt Physics; Jolt types never appear in these headers).
//
//   Collider       a collision shape (box, sphere, capsule, cylinder, convex
//                  hull, triangle mesh, heightfield), cheap to copy and share
//   Rigidbody      component: makes a GameObject a static, kinematic or dynamic body
//   CharacterController  component: walking character (slopes, steps, pushing)
//   PhysicsWorld   the simulation: stepping, gravity, raycasts
//
//   auto& crate = scene.spawn("Crate");
//   crate.addComponent<ve::MeshRenderer>(cubeMesh, woodMaterial);
//   crate.addComponent<ve::Rigidbody>(ve::Collider::box({0.5f, 0.5f, 0.5f}));   // dynamic by default
//
//   if (auto hit = physics().raycast(eye, forward, 50.0f)) log::info("looking at {}", hit->gameObject->name());
//
// The world steps once per fixed update (after the scene's fixedUpdate hooks).
// Dynamic bodies write their pose back to their Transform, interpolated
// between steps for smooth motion.
// =============================================================================

#include "ve/scene/GameObject.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace ve {

struct MeshData;
class Scene;
class Rigidbody;

// A collision shape. Create with the static functions (after the Engine exists).
// Shapes are immutable and reference counted, so share one Collider between
// many bodies (e.g. every rock of the same variant).
class Collider {
public:
    Collider() = default;

    static Collider box(const glm::vec3& halfExtents);
    static Collider sphere(float radius);
    // Capsule / cylinder along the Y axis, centred on the origin.
    static Collider capsule(float halfHeight, float radius);
    static Collider cylinder(float halfHeight, float radius);
    // Smallest convex shape around the points (rocks, crates, props).
    static Collider convexHull(const std::vector<glm::vec3>& points);
    static Collider convexHull(const MeshData& mesh);
    // Exact triangle mesh. Static/kinematic bodies only (can't be dynamic).
    static Collider mesh(const MeshData& mesh);
    // Terrain: samplesPerSide^2 heights (row-major, z rows, x columns),
    // world position = origin + (x * cellSize, height, z * cellSize).
    static Collider heightfield(const std::vector<float>& heights, uint32_t samplesPerSide, float cellSize,
                                const glm::vec3& origin);

    // The same shape moved/rotated relative to the body origin.
    Collider offset(const glm::vec3& translation, const glm::quat& rotation = glm::quat(1, 0, 0, 0)) const;

    bool valid() const { return impl != nullptr; }

    struct Impl; // engine internal (holds the Jolt shape)
    std::shared_ptr<const Impl> impl;
};

enum class BodyType {
    Static,    // never moves (terrain, buildings, trees)
    Kinematic, // moved by code (follows its Transform), pushes dynamic bodies
    Dynamic,   // simulated: gravity, collisions, forces
};

struct RaycastHit {
    glm::vec3 point{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    float distance = 0.0f;
    Rigidbody* body = nullptr;          // null when a character was hit
    GameObject* gameObject = nullptr;
};

class PhysicsWorld {
public:
    PhysicsWorld();
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    void init();
    void shutdown();

    glm::vec3 gravity() const;
    void setGravity(const glm::vec3& g);

    // Closest hit along a ray (direction is normalised for you).
    // `ignore` skips one body (e.g. the thrower); characters are ignored
    // unless hitCharacters is true.
    std::optional<RaycastHit> raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
                                      const Rigidbody* ignore = nullptr, bool hitCharacters = false) const;

    uint32_t bodyCount() const;
    bool paused = false;
    float lastStepMs = 0.0f; // CPU time of the last step (shown in the F1 overlay)

    // --- Engine internal ---------------------------------------------------------
    void step(Scene& scene, float dt);           // once per fixed update
    void interpolate(Scene& scene, float alpha); // after the fixed updates of a frame

    struct Impl;
    Impl& impl() { return *m_impl; }

private:
    std::unique_ptr<Impl> m_impl;
};

class Rigidbody : public Component {
public:
    explicit Rigidbody(Collider collider = {}, BodyType type = BodyType::Dynamic);
    ~Rigidbody() override;

    // --- Settings (read when the body is created in start()) ---------------------
    Collider collider;
    BodyType type;
    float mass = 0.0f;         // kg; 0 = computed from the shape's volume (density 1000 kg/m^3)
    float friction = 0.6f;
    float restitution = 0.1f;  // bounciness 0..1
    float linearDamping = 0.05f;
    float angularDamping = 0.05f;
    bool continuousCollision = false; // for fast objects (projectiles): no tunnelling
    bool interpolate = true;          // smooth the Transform between fixed steps

    // --- Runtime (dynamic bodies) ------------------------------------------------------
    void addForce(const glm::vec3& force);     // N, applied for the next step
    void addImpulse(const glm::vec3& impulse); // N*s, instant velocity change
    void addImpulseAt(const glm::vec3& impulse, const glm::vec3& worldPoint);
    glm::vec3 linearVelocity() const;
    void setLinearVelocity(const glm::vec3& v);
    glm::vec3 angularVelocity() const;
    void setAngularVelocity(const glm::vec3& w);
    // Moves the body instantly (and its Transform), resetting interpolation.
    void teleport(const glm::vec3& position, const glm::quat& rotation);
    // Static bodies: call after moving the Transform from code.
    void syncFromTransform();
    bool isAwake() const; // dynamic body currently simulating (not asleep)
    bool created() const { return m_bodyId != kInvalidBody; }

    void start() override;
    void onDestroy() override;

    // Engine internal.
    static constexpr uint32_t kInvalidBody = 0xFFFFFFFFu;
    uint32_t bodyId() const { return m_bodyId; }
    glm::vec3 m_prevPos{0.0f}, m_currPos{0.0f};
    glm::quat m_prevRot{1, 0, 0, 0}, m_currRot{1, 0, 0, 0};

private:
    void destroyBody();
    uint32_t m_bodyId = kInvalidBody;
    glm::vec3 m_pendingVelocity{0.0f};
    bool m_hasPendingVelocity = false;
};

// A "virtual" character: not simulated as a rigid body, but moved by code
// with collision response (slides along walls, walks up slopes and steps,
// sticks to the floor going downhill, pushes dynamic bodies). Its position is
// its FEET. Call move() from fixedUpdate().
class CharacterController : public Component {
public:
    CharacterController();
    ~CharacterController() override;

    // --- Settings (read in start()) ---------------------------------------------------
    float radius = 0.35f;
    float height = 1.8f;           // total height
    float maxSlopeDegrees = 50.0f; // steeper is a wall
    float stepHeight = 0.4f;       // max stair/rock step
    float mass = 70.0f;
    float pushStrength = 120.0f;   // max force (N) used to push dynamic bodies
    bool interpolate = true;

    // Moves the character for one fixed step. `horizontalVelocity` is the
    // desired walking velocity (m/s, world XZ); gravity is applied for you.
    void move(const glm::vec3& horizontalVelocity, float dt);
    void jump(float speed) { m_jumpSpeed = speed; } // applied on the next move() if grounded

    bool grounded() const;
    bool onSteepGround() const;
    glm::vec3 velocity() const;
    glm::vec3 groundNormal() const;
    // Moves the character instantly (also use this every frame to drag it
    // along while the player is flying/noclipping).
    void teleport(const glm::vec3& feet);

    void start() override;
    void onDestroy() override;

    // Engine internal.
    glm::vec3 m_prevPos{0.0f}, m_currPos{0.0f};
    struct Impl;

private:
    void destroy();
    std::unique_ptr<Impl> m_impl;
    float m_jumpSpeed = 0.0f;
};

} // namespace ve
