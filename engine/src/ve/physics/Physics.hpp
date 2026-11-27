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
//   auto hits = physics().overlapSphere(swordTip, 0.4f);           // melee, AI senses, placement checks
//   auto hit = physics().sphereCast(eye, 0.3f, forward, 3.0f);     // a fat ray
//
// Events: override these in any Component (or GameObject subclass) on an
// object with a Rigidbody (or a CharacterController):
//   void onCollisionEnter(const ve::Collision& c) override;  // c.other, c.point, c.normal, c.impactSpeed
//   void onCollisionStay(const ve::Collision& c) override;   // every step while touching
//   void onCollisionExit(const ve::Collision& c) override;   // also when the bodies fall asleep
//   void onTriggerEnter(ve::GameObject& other) override;     // Rigidbody::isTrigger volumes
//   void onTriggerExit(ve::GameObject& other) override;
// They're sent after each physics step, on the main thread; destroying objects
// from them is fine (destruction is deferred to the end of the frame).
//
// The world steps once per fixed update (after the scene's fixedUpdate hooks).
// Dynamic bodies write their pose back to their Transform, interpolated
// between steps for smooth motion.
// =============================================================================

#include "ve/scene/GameObject.hpp"
#include "ve/scene/ObjectRef.hpp"
#include "ve/scene/SaveGame.hpp"

#include <glm/glm.hpp>
#include <initializer_list>
#include <string>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace ve {

struct MeshData;
class Mesh;
class Scene;
class Rigidbody;
class CharacterController;

/// A collision shape. Create with the static functions (after the Engine exists).
/// Shapes are immutable and reference counted, so share one Collider between
/// many bodies (e.g. every rock of the same variant).
class Collider {
public:
    Collider() = default;

    /// A box, centred on the origin. @param halfExtents half the size along each axis (m).
    static Collider box(const glm::vec3& halfExtents);
    /// A sphere centred on the origin. @param radius in metres.
    static Collider sphere(float radius);
    /// Capsule / cylinder along the Y axis, centred on the origin.
    static Collider capsule(float halfHeight, float radius);
    /// A cylinder along Y, centred on the origin. @param halfHeight half the height of the straight part (m).
    static Collider cylinder(float halfHeight, float radius);
    /// Smallest convex shape around the points (rocks, crates, props).
    static Collider convexHull(const std::vector<glm::vec3>& points);
    /// The convex hull of a mesh's vertices.
    static Collider convexHull(const MeshData& mesh);
    /// Exact triangle mesh. Static/kinematic bodies only (can't be dynamic).
    static Collider mesh(const MeshData& mesh);
    /// Terrain: samplesPerSide^2 heights (row-major, z rows, x columns),
    /// world position = origin + (x * cellSize, height, z * cellSize).
    static Collider heightfield(const std::vector<float>& heights, uint32_t samplesPerSide, float cellSize,
                                const glm::vec3& origin);

    /// The same shape moved/rotated relative to the body origin.
    Collider offset(const glm::vec3& translation, const glm::quat& rotation = glm::quat(1, 0, 0, 0)) const;

    /// Whether this holds a shape (a default-constructed Collider doesn't).
    bool valid() const { return impl != nullptr; }
    /// The shape as world triangles (3 points each, counter-clockwise seen from
    /// outside; round shapes are tessellated) under `transform` (may scale).
    /// Used to build navmeshes.
    std::vector<glm::vec3> triangles(const glm::mat4& transform) const;

    struct Impl; ///< engine internal (holds the Jolt shape)
    std::shared_ptr<const Impl> impl;
};

enum class BodyType {
    Static,    ///< never moves (terrain, buildings, trees)
    Kinematic, ///< moved by code (follows its Transform), pushes dynamic bodies
    Dynamic,   ///< simulated: gravity, collisions, forces
};

/// A hit from a raycast, shape cast or overlap query.
struct RaycastHit {
    glm::vec3 point{0.0f};  ///< World-space point of the hit.
    glm::vec3 normal{0.0f, 1.0f, 0.0f};   ///< surface normal of what was hit
    float distance = 0.0f;                 ///< along the ray / cast (0 for overlaps, or casts that start inside)
    Rigidbody* body = nullptr;             ///< null for characters and createStaticBody() bodies
    GameObject* gameObject = nullptr;      ///< null for createStaticBody() bodies
    uint32_t userValue = 0;                ///< the value given to createStaticBody()
    int layer = 0;                         ///< its collision layer (PhysicsWorld::addLayer)
    std::string surface;                   ///< its physics material's surface ("" = none set)
};

/// What a query can hit.
struct QueryFilter {
    bool statics = true;       ///< static bodies (terrain, buildings, foliage colliders)
    bool dynamics = true;      ///< dynamic and kinematic bodies
    bool characters = false;   ///< CharacterController capsules
    bool triggers = false;     ///< Rigidbody::isTrigger volumes
    uint32_t layers = 0xFFFFFFFFu; ///< collision layers it can hit (bit per layer, PhysicsWorld::layerMask)
    const Rigidbody* ignore = nullptr;  ///< A body to skip (e.g. the one doing the query).
    const CharacterController* ignoreCharacter = nullptr;  ///< A character to skip (e.g. the player's own capsule).
};

/// A contact, as seen from one of the two objects.
struct Collision {
    GameObject* other = nullptr;     ///< null for createStaticBody() bodies (foliage)
    Rigidbody* otherBody = nullptr;  ///< null for characters and createStaticBody() bodies
    uint32_t otherUserValue = 0;     ///< createStaticBody()'s userValue
    glm::vec3 point{0.0f};           ///< world-space contact point
    glm::vec3 normal{0.0f, 1.0f, 0.0f}; ///< pointing from `other` towards this object
    float impactSpeed = 0.0f;        ///< closing speed along the normal when the contact began (m/s)
    int otherLayer = 0;              ///< the other's collision layer
    std::string otherSurface;        ///< the other's surface ("" = none set): impact sounds, effects
};

/// The physics simulation (Jolt): bodies, characters, joints, layers, and
/// queries (raycasts, shape casts, overlaps). Engine::physics(); main thread,
/// except where noted. Steps once per fixed update.
class PhysicsWorld {
public:
    PhysicsWorld();
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    /// Engine: creates the physics system.
    void init();
    /// Engine: destroys every body and the physics system.
    void shutdown();

    /// Gravity in m/s^2 (default 0, -9.81, 0).
    glm::vec3 gravity() const;
    /// Sets gravity (m/s^2) for every dynamic body and character.
    void setGravity(const glm::vec3& g);

    /// Closest hit along a ray (direction is normalised for you).
    /// `ignore` skips one body (e.g. the thrower); characters are ignored
    /// unless hitCharacters is true.
    std::optional<RaycastHit> raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
                                      const Rigidbody* ignore = nullptr, bool hitCharacters = false) const;
    /// Closest hit along a ray, with full control over what it can hit.
    std::optional<RaycastHit> raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
                                      const QueryFilter& filter) const;

    /// Sweeps a shape along `direction` and returns the first hit. A cast that
    /// starts inside something hits it at distance 0 (normal = way out).
    std::optional<RaycastHit> sphereCast(const glm::vec3& origin, float radius, const glm::vec3& direction,
                                         float maxDistance, const QueryFilter& filter = {}) const;
    /// Like sphereCast, with a box (`halfExtents`, rotated by `rotation`) centred at `center`.
    std::optional<RaycastHit> boxCast(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation,
                                      const glm::vec3& direction, float maxDistance, const QueryFilter& filter = {}) const;
    /// Like sphereCast, with any Collider placed at `position` / `rotation`.
    std::optional<RaycastHit> shapeCast(const Collider& shape, const glm::vec3& position, const glm::quat& rotation,
                                        const glm::vec3& direction, float maxDistance, const QueryFilter& filter = {}) const;

    /// Everything touching a shape (one hit per body, `point` = deepest point).
    std::vector<RaycastHit> overlapSphere(const glm::vec3& center, float radius, const QueryFilter& filter = {}) const;
    /// Everything touching a box (half extents, rotation) centred at `center`.
    std::vector<RaycastHit> overlapBox(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation,
                                       const QueryFilter& filter = {}) const;
    /// Everything touching any Collider placed at `position` / `rotation`.
    std::vector<RaycastHit> overlap(const Collider& shape, const glm::vec3& position, const glm::quat& rotation,
                                    const QueryFilter& filter = {}) const;

    /// Static bodies without a GameObject: for instanced props (trees, rocks)
    /// where a GameObject per instance would be wasteful. `userValue` comes
    /// back in RaycastHit::userValue (e.g. a prop type or instance id).
    uint32_t createStaticBody(const Collider& collider, const glm::vec3& position,
                              const glm::quat& rotation = glm::quat(1, 0, 0, 0), const glm::vec3& scale = glm::vec3(1.0f),
                              uint32_t userValue = 0, const std::string& surface = {}, int layer = 0);

    // --- Collision layers -----------------------------------------------------------
    /// Up to 32 named layers; "Default" (0) always exists and everything is in
    /// it unless told otherwise. Every pair collides until setCollision(a, b,
    /// false). Bodies pick theirs by name (Rigidbody::layer,
    /// CharacterController::layer); queries by mask (QueryFilter::layers).
    ///   int debris = physics().addLayer("Debris");
    ///   physics().setCollision(debris, physics().layer("Characters"), false);
    int addLayer(const std::string& name);           ///< its index (the existing one if the name is taken; -1 when full)
    int layer(const std::string& name) const;        ///< -1 if there's no such layer
    /// A layer's name ("" for an unused index).
    const std::string& layerName(int index) const;
    /// Number of layers in use.
    int layerCount() const;
    void setCollision(int a, int b, bool collide);   ///< both ways; affects new contacts
    /// Whether bodies in layers `a` and `b` collide.
    bool collides(int a, int b) const;
    uint32_t layerMask(std::initializer_list<const char*> names) const; ///< for QueryFilter::layers
    /// Maximum number of collision layers.
    static constexpr int kMaxLayers = 32;
    /// Removes a body made by createStaticBody.
    void destroyStaticBody(uint32_t bodyId);

    /// Bodies in the world (for stats).
    uint32_t bodyCount() const;
    /// Wireframes of the bodies within `radius` (ve::debug lines): static grey,
    /// dynamic orange (darker asleep), kinematic blue, triggers green,
    /// characters yellow; terrain heightfields only when asked. Bodies around
    /// `eye` (the camera's own character) are left out.
    void drawDebug(const glm::vec3& center, float radius, bool bodies, bool terrain, bool characters, const glm::vec3& eye) const;
    bool paused = false;  ///< Stops stepping the simulation (bodies freeze; queries still work).
    float lastStepMs = 0.0f; ///< CPU time of the last step (shown in the F1 overlay)

    /// An exact static mesh collider (Collider::mesh(data)) for a shared mesh,
    /// built on first use and shared by every caller while the mesh lives (e.g.
    /// every instance of a model with staticColliders). `data` is its CPU copy.
    Collider meshCollider(const std::shared_ptr<const Mesh>& mesh, const MeshData& data);

    // --- Engine internal ---------------------------------------------------------
    void step(Scene& scene, float dt);           ///< once per fixed update
    void interpolate(Scene& scene, float alpha); ///< after the fixed updates of a frame

    struct Impl; ///< (Jolt state: ve/physics/JoltInternal.hpp, physics .cpp files only)
    Impl& impl() { return *m_impl; }

private:
    std::unique_ptr<Impl> m_impl;
    struct MeshColliderCache; ///< meshCollider()'s (PhysicsWorld.cpp)
    std::shared_ptr<MeshColliderCache> m_meshColliders;
};

/// Simple collider shapes described by numbers (editable in the editor and
/// saved in scene files; a Collider itself is an opaque physics shape).
enum class ColliderShape { Custom, Box, Sphere, Capsule, Cylinder };

/// Makes its object a physics body: static, kinematic (moved by code) or
/// dynamic (simulated: gravity, collisions, forces). Dynamic bodies drive the
/// object's Transform; created in start() from the settings below.
class Rigidbody : public Component {
public:
    /// A body with `collider` (or set `shape` instead) of the given type.
    explicit Rigidbody(Collider collider = {}, BodyType type = BodyType::Dynamic);
    ~Rigidbody() override;

    // --- Settings (read when the body is created in start()) ---------------------
    Collider collider;  ///< The shape; takes precedence over `shape` when valid.
    BodyType type;  ///< Static, kinematic or dynamic.
    /// When shape != Custom (and no collider was given), the collider is built from
    /// these: Box = size (full extents); Sphere = size.x diameter; Capsule /
    /// Cylinder = size.x diameter, size.y total height (along Y). offset moves it.
    ColliderShape shape = ColliderShape::Custom;
    glm::vec3 shapeSize{1.0f};  ///< See above: Box = full size; Sphere / Capsule / Cylinder: x = diameter, y = height.
    glm::vec3 shapeOffset{0.0f};  ///< Moves the generated shape relative to the object (m, local space).
    /// Re-creates the body from the current settings (editor edits).
    void rebuild();
    float mass = 0.0f;         ///< kg; 0 = computed from the shape's volume (density 1000 kg/m^3)
    float friction = 0.6f;  ///< Surface friction (0 = ice, 1 = rubber-like).
    float restitution = 0.1f;  ///< bounciness 0..1
    float linearDamping = 0.05f;  ///< Slows linear motion over time (air drag; 0 = none).
    float angularDamping = 0.05f;  ///< Slows spinning over time (0 = none).
    bool continuousCollision = false; ///< for fast objects (projectiles): no tunnelling
    std::string layer = "Default";    ///< collision layer (PhysicsWorld::addLayer; read when the body is created)
    std::string surface;              ///< physics material's surface ("Wood", "Stone"...): in hits, contacts, footsteps
    /// A trigger volume: no collision response, sends onTriggerEnter/Exit when
    /// bodies or characters enter or leave it (pickups, damage zones, doors).
    /// Use a convex shape (box, sphere, capsule, hull); Static triggers still
    /// detect everything, including characters and sleeping bodies.
    bool isTrigger = false;
    bool interpolate = true;          ///< smooth the Transform between fixed steps

    // --- Runtime (dynamic bodies) ------------------------------------------------------
    void addForce(const glm::vec3& force);     ///< N, applied for the next step
    void addImpulse(const glm::vec3& impulse); ///< N*s, instant velocity change
    /// Adds an impulse (N*s) at a world point: pushes and spins it (a hit off-centre).
    void addImpulseAt(const glm::vec3& impulse, const glm::vec3& worldPoint);
    /// Velocity in m/s (world space).
    glm::vec3 linearVelocity() const;
    /// Sets the velocity in m/s (applied right away, or on creation if called before start()).
    void setLinearVelocity(const glm::vec3& v);
    /// Angular velocity in radians/s (world-space axis * speed).
    glm::vec3 angularVelocity() const;
    /// Sets the angular velocity (radians/s around each world axis).
    void setAngularVelocity(const glm::vec3& w);
    /// Moves the body instantly (and its Transform), resetting interpolation.
    void teleport(const glm::vec3& position, const glm::quat& rotation);
    /// Static bodies: call after moving the Transform from code.
    void syncFromTransform();
    bool isAwake() const; ///< dynamic body currently simulating (not asleep)
    /// Whether the physics body exists (after start(), until destroyed).
    bool created() const { return m_bodyId != kInvalidBody; }

    void start() override;
    void onDestroy() override;

    /// Engine internal.
    static constexpr uint32_t kInvalidBody = 0xFFFFFFFFu;
    /// The Jolt body id (engine internal), or kInvalidBody.
    uint32_t bodyId() const { return m_bodyId; }
    glm::vec3 m_prevPos{0.0f};             ///< Internal: position at the previous step (interpolation).
    glm::vec3 m_currPos{0.0f};             ///< Internal: position at the last step.
    glm::quat m_prevRot{1, 0, 0, 0};       ///< Internal: rotation at the previous step.
    glm::quat m_currRot{1, 0, 0, 0};       ///< Internal: rotation at the last step.

private:
    void destroyBody();
    uint32_t m_bodyId = kInvalidBody;
    bool m_startedOnce = false;
    glm::vec3 m_pendingVelocity{0.0f};
    bool m_hasPendingVelocity = false;
};

/// A "virtual" character: not simulated as a rigid body, but moved by code
/// with collision response (slides along walls, walks up slopes and steps,
/// sticks to the floor going downhill, pushes dynamic bodies). Its position is
/// its FEET. Call move() from fixedUpdate().
class CharacterController : public Component {
public:
    CharacterController();
    ~CharacterController() override;

    // --- Settings (read in start()) ---------------------------------------------------
    float radius = 0.35f;  ///< Capsule radius in metres.
    float height = 1.8f;           ///< total height
    float maxSlopeDegrees = 50.0f; ///< steeper is a wall
    float stepHeight = 0.4f;       ///< max stair/rock step
    float mass = 70.0f;  ///< kg: how hard it pushes and is pushed by dynamic bodies.
    float pushStrength = 120.0f;   ///< max force (N) used to push dynamic bodies
    bool interpolate = true;  ///< Smooth the Transform between fixed steps.
    std::string layer = "Default"; ///< collision layer of the capsule (what it bumps into, what hits it)
    std::string surface;           ///< what others hit (e.g. "Flesh")

    /// Moves the character for one fixed step. `horizontalVelocity` is the
    /// desired walking velocity (m/s, world XZ); gravity is applied for you.
    void move(const glm::vec3& horizontalVelocity, float dt);
    void jump(float speed) { m_jumpSpeed = speed; } ///< applied on the next move() if grounded

    /// Standing on walkable ground (not steeper than maxSlopeDegrees).
    bool grounded() const;
    /// Touching ground too steep to stand on (sliding down).
    bool onSteepGround() const;
    /// Current velocity in m/s, gravity included.
    glm::vec3 velocity() const;
    /// Normal of the ground under it (up when airborne).
    glm::vec3 groundNormal() const;
    /// The surface of what it stands on ("" = nothing / none set): footsteps.
    /// groundObject: that body's GameObject (null for terrain-less static bodies).
    std::string groundSurface() const;
    /// The GameObject of the body it stands on (null for terrain and createStaticBody bodies).
    GameObject* groundObject() const;
    uint32_t groundUserValue() const; ///< createStaticBody()'s userValue of the ground (foliage, rocks)
    /// Moves the character instantly (also use this every frame to drag it
    /// along while the player is flying/noclipping).
    void teleport(const glm::vec3& feet);

    void start() override;
    void onDestroy() override;

    /// Engine internal.
    uint32_t innerBodyId() const; ///< the capsule body bodies collide with (kInvalidBody before start())
    glm::vec3 m_prevPos{0.0f};             ///< Internal: feet position at the previous step (interpolation).
    glm::vec3 m_currPos{0.0f};             ///< Internal: feet position at the last step.
    struct Impl;

private:
    void destroy();
    std::unique_ptr<Impl> m_impl;
    float m_jumpSpeed = 0.0f;
};


/// Joints: a Rigidbody held to another one (connectedTo / connect()) or to the
/// world. Add one of the subclasses next to the Rigidbody; it's created once
/// both bodies exist (and again if they're rebuilt).
///
///   auto& hinge = door.addComponent<ve::HingeJoint>();       // swings on its post
///   hinge.anchor = {-0.5f, 0, 0}; hinge.axis = {0, 1, 0};
///   hinge.useLimits = true; hinge.limitMin = 0; hinge.limitMax = 100;   // degrees
///   hinge.motor = ve::Joint::Motor::Position; hinge.motorTarget = 90;    // swings open
///
///   auto& weld = plank.addComponent<ve::FixedJoint>(); weld.connect(&post);
///   weld.breakForce = 4000; weld.onBreak = [](ve::Joint&) { ... };       // snaps
///
/// Hinge: angle in degrees around `axis`; Slider: position in metres along it;
/// Distance: a rope / spring between `anchor` and `connectedAnchor`; Fixed:
/// welded. The anchor is in this object's space, connectedAnchor in the other's
/// (the world's if none); axis in this object's space. Units: N, N m, degrees.
class Joint : public Component, public Saveable {
public:
    /// Which kind of joint (fixed by the subclass).
    enum class Type { Fixed, Hinge, Slider, Distance };
    /// Motor drive: off, towards a velocity, or towards a position.
    enum class Motor { Off, Velocity, Position };

    ~Joint() override;
    /// The joint's kind.
    Type type() const { return m_type; }

    std::string connectedTo;          ///< the other body's object name (scene files); empty = the world
    void connect(GameObject* other);  ///< from code (null = the world)
    glm::vec3 anchor{0.0f};  ///< Pivot point in this object's local space (m).
    glm::vec3 connectedAnchor{0.0f};  ///< Distance (others use the anchor's world point)
    glm::vec3 axis{0.0f, 1.0f, 0.0f}; ///< Hinge / Slider

    bool useLimits = false;  ///< Restrict the joint to [limitMin, limitMax].
    float limitMin = -90.0f;          ///< Lower limit. Hinge: degrees; Slider: metres.
    float limitMax = 90.0f;           ///< Upper limit. Hinge: degrees; Slider: metres.
    float minDistance = -1.0f;        ///< Distance joint: shortest length (m); < 0 = the length when created; 0 = a rope.
    float maxDistance = -1.0f;        ///< Distance joint: longest length (m); < 0 = the length when created.
    float springFrequency = 0.0f;     ///< Hz: soft limits / a springy distance (0 = rigid)
    float springDamping = 0.5f;  ///< Spring damping ratio (0 = oscillates, 1 = critically damped).
    float friction = 0.0f;            ///< Hinge: N m, Slider: N (resists free movement)

    Motor motor = Motor::Off;  ///< Motor mode (Hinge / Slider).
    float motorTarget = 0.0f;         ///< Velocity: deg/s or m/s; Position: degrees or metres
    float motorMaxForce = 500.0f;     ///< N m (hinge) / N (slider)
    float motorFrequency = 4.0f;      ///< Position motors: how stiffly they track (Hz)

    float breakForce = 0.0f;          ///< > 0: breaks when the joint's force exceeds this (N).
    float breakTorque = 0.0f;         ///< > 0: breaks when its torque exceeds this (N m).
    std::function<void(Joint&)> onBreak;  ///< Called when it breaks (then broken() is true).

    bool attached() const;            ///< the constraint exists
    /// Whether it broke (breakForce / breakTorque exceeded); repair() re-attaches it.
    bool broken() const { return m_broken; }
    void repair() { m_broken = false; } ///< (attaches again next step)
    float angle() const;              ///< Hinge: degrees
    float position() const;           ///< Slider: metres; Distance: current length
    float lastForce() const { return m_force; }   ///< N, last step
    float lastTorque() const { return m_torque; } ///< N m, last step

    void fixedUpdate(float dt) override;
    void onDestroy() override;
    void saveState(Json& out) const override { out = {{"broken", m_broken}}; }
    void loadState(const Json& in) override;

    /// Engine internal.
    struct Impl;
    void detach();                    ///< the joint breaks / goes
    void detachIfUsing(uint32_t bodyId); ///< a body is going away
    void afterStep(float dt);         ///< forces, breaking (PhysicsWorld::step)
    /// Draws the joint's anchors and axis (debug channel "Joints").
    void drawDebug() const;

protected:
    explicit Joint(Type t); ///< (out of line: Impl is complete only in Joints.cpp)

private:
    bool attach();
    void applyMotor();
    GameObject* connectedObject() const; ///< the remembered other object, if it's still alive
    Type m_type;
    ObjectRef<> m_connected; ///< the other object once found (connect() or by name); empty = none / the world
    bool m_broken = false;
    uint8_t m_findWait = 0; ///< fixed steps until attach() searches for `connectedTo` by name again
    float m_force = 0.0f, m_torque = 0.0f;
    std::unique_ptr<Impl> m_impl;
};

/// Welds two bodies together (or one to the world).
class FixedJoint : public Joint {
public:
    FixedJoint() : Joint(Type::Fixed) {}
};
/// Rotates around `axis` through `anchor`: doors, wheels, levers.
class HingeJoint : public Joint {
public:
    HingeJoint() : Joint(Type::Hinge) {}
};
/// Slides along `axis`: drawers, pistons, lifts.
class SliderJoint : public Joint {
public:
    SliderJoint() : Joint(Type::Slider) { axis = {1.0f, 0.0f, 0.0f}; limitMin = -1.0f; limitMax = 1.0f; }
};
/// Keeps two anchors within [minDistance, maxDistance]: ropes, chains, springs.
class DistanceJoint : public Joint {
public:
    DistanceJoint() : Joint(Type::Distance) {}
};

class TypeRegistry;
/// Reflection (scene files, editor): this module's components. The Engine calls it at startup.
void registerPhysicsTypes(TypeRegistry& r);

} // namespace ve
