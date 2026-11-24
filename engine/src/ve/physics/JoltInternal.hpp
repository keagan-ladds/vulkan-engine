#pragma once

// =============================================================================
// INTERNAL to the physics module: the Jolt Physics backend's shared pieces.
//
// RULE: only ve/physics/*.cpp may include this file (and Jolt headers). Never
// include it from a header (Physics.hpp included) or from another module:
// Jolt is linked PRIVATE and games must never see its types (decision D13).
//
// What's here: the Jolt includes every physics .cpp needs, glm <-> Jolt
// conversions, the collision layer tables, Collider::Impl, PhysicsWorld::Impl
// (with the contact queue it owns) and small helpers shared between the files.
// Key Jolt concepts used by the module:
//   PhysicsSystem       the world: bodies, broad phase, constraints
//   ObjectLayer         what kind of object a body is (static, moving, character)
//   BroadPhaseLayer     coarse acceleration-structure buckets (static vs moving)
//   Shape               immutable, ref-counted collision geometry (our Collider)
//   BodyInterface       thread-safe API to create/move/query bodies by BodyID
//   CharacterVirtual    a kinematic capsule moved by queries (our CharacterController)
//
// The module's files:
//   PhysicsWorld.cpp         Jolt setup, step, contacts, interpolation, layers, static bodies, mesh-collider cache
//   Collider.cpp             shapes, Collider::triangles
//   Rigidbody.cpp            the Rigidbody component
//   CharacterController.cpp  the CharacterController component
//   Joints.cpp               Joint components (Jolt constraints)
//   PhysicsQueries.cpp       raycasts, shape casts, overlaps
//   PhysicsDebug.cpp         debug drawing (the only file that uses ve/debug)
//   PhysicsReflection.cpp    registerPhysicsTypes (no Jolt)
// =============================================================================

#include "ve/physics/Physics.hpp"

#include "ve/core/Engine.hpp"

// Jolt.h must come first.
#include <Jolt/Jolt.h>

#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ve {

// (A named namespace, not an anonymous one: these are shared between the
// module's .cpp files.)
namespace physics_detail {

// ---- glm <-> Jolt conversions -----------------------------------------------------
inline JPH::Vec3 toJ(const glm::vec3& v) { return JPH::Vec3(v.x, v.y, v.z); }
inline JPH::Quat toJ(const glm::quat& q) { return JPH::Quat(q.x, q.y, q.z, q.w); }
inline glm::vec3 toG(JPH::Vec3Arg v) { return {v.GetX(), v.GetY(), v.GetZ()}; }
inline glm::quat toG(JPH::QuatArg q) { return glm::quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ()); }

// ---- Collision layers ------------------------------------------------------------
// Object layers say what collides with what; broad-phase layers group bodies
// into separate acceleration trees (static bodies rarely change, so they get
// their own tree that's cheap to keep optimised).
// A Jolt object layer = a kind (low 2 bits: how the engine treats it) + the
// game's collision layer (PhysicsWorld::addLayer, the bits above).
namespace Layers {
constexpr JPH::ObjectLayer NonMoving = 0; ///< static bodies
constexpr JPH::ObjectLayer Moving = 1;    ///< dynamic + kinematic bodies
constexpr JPH::ObjectLayer Character = 2; ///< the capsule that lets bodies hit characters
constexpr JPH::ObjectLayer Sensor = 3;    ///< trigger volumes (Rigidbody::isTrigger)
} // namespace Layers
inline JPH::ObjectLayer kindOf(JPH::ObjectLayer l) { return JPH::ObjectLayer(l & 3u); }
inline int gameLayerOf(JPH::ObjectLayer l) { return int(l >> 2); }
inline JPH::ObjectLayer makeLayer(JPH::ObjectLayer kind, int gameLayer) { return JPH::ObjectLayer(kind | (uint32_t(gameLayer & 31) << 2)); }

/// The game's layers: names and who collides with whom (a bit per layer).
struct LayerTable {
    std::vector<std::string> names{"Default"};
    uint32_t collide[32];
    LayerTable() { std::fill(std::begin(collide), std::end(collide), 0xFFFFFFFFu); }
    bool pair(int a, int b) const { return (collide[a & 31] >> (b & 31)) & 1u; }
};
namespace BPLayers {
constexpr JPH::BroadPhaseLayer NonMoving(0);
constexpr JPH::BroadPhaseLayer Moving(1);
constexpr uint32_t Count = 2;
} // namespace BPLayers

class BroadPhaseLayerMap final : public JPH::BroadPhaseLayerInterface {
public:
    uint32_t GetNumBroadPhaseLayers() const override { return BPLayers::Count; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override
    {
        return kindOf(layer) == Layers::NonMoving ? BPLayers::NonMoving : BPLayers::Moving;
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override
    {
        return layer == BPLayers::NonMoving ? "NonMoving" : "Moving";
    }
#endif
};

class ObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer bp) const override
    {
        // Static things, characters and triggers only need to test against moving things.
        if (kindOf(layer) == Layers::Moving) return true;
        return bp == BPLayers::Moving;
    }
};

class ObjectPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer la, JPH::ObjectLayer lb) const override
    {
        if (table && !table->pair(gameLayerOf(la), gameLayerOf(lb))) return false; // the game's matrix
        const JPH::ObjectLayer a = kindOf(la), b = kindOf(lb);
        if (a == Layers::NonMoving) return b == Layers::Moving;
        if (a == Layers::Moving) return true;
        if (a == Layers::Character) return b == Layers::Moving || b == Layers::Sensor; // hit by bodies, enters triggers
        return b == Layers::Moving || b == Layers::Character;                         // triggers
    }
    const LayerTable* table = nullptr;
};

/// Collects contact callbacks (from Jolt's worker threads) for main-thread dispatch.
class ContactQueue final : public JPH::ContactListener {
public:
    enum class Kind : uint8_t { Added, Persisted, Removed };
    struct Event {
        Kind kind;
        bool sensor;
        JPH::BodyID a, b; ///< a < b
        glm::vec3 point{0.0f};
        glm::vec3 normal{0.0f}; ///< from a to b
        float speed = 0.0f;
    };

    void OnContactAdded(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& m, JPH::ContactSettings&) override
    {
        push(Kind::Added, a, b, m);
    }
    void OnContactPersisted(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& m,
                            JPH::ContactSettings&) override
    {
        push(Kind::Persisted, a, b, m);
    }
    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override
    {
        std::lock_guard lock(mutex);
        events.push_back({Kind::Removed, false, pair.GetBody1ID(), pair.GetBody2ID()});
    }

    std::mutex mutex;
    std::vector<Event> events;

private:
    void push(Kind kind, const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& m)
    {
        Event e{kind, a.IsSensor() || b.IsSensor(), a.GetID(), b.GetID()};
        const JPH::RVec3 p = m.GetWorldSpaceContactPointOn1(0);
        e.point = glm::vec3(float(p.GetX()), float(p.GetY()), float(p.GetZ()));
        e.normal = toG(m.mWorldSpaceNormal);
        if (kind == Kind::Added) {
            // Closing speed before the solver runs (bodies are locked: no lock calls here).
            const JPH::Vec3 rel = b.GetPointVelocity(p) - a.GetPointVelocity(p);
            e.speed = std::max(0.0f, -rel.Dot(m.mWorldSpaceNormal));
        }
        std::lock_guard lock(mutex);
        events.push_back(e);
    }
};

/// Jolt's global state (allocator, factory, types) is set up: PhysicsWorld::init / shutdown.
inline bool g_joltInitialized = false;

} // namespace physics_detail

struct Collider::Impl {
    JPH::RefConst<JPH::Shape> shape;
};

struct PhysicsWorld::Impl {
    std::unique_ptr<JPH::TempAllocatorImpl> temp;
    std::unique_ptr<JPH::JobSystemThreadPool> jobs;
    physics_detail::BroadPhaseLayerMap broadPhaseLayers;
    physics_detail::ObjectVsBroadPhaseFilter objectVsBroadPhase;
    physics_detail::ObjectPairFilter objectPairs;
    std::unique_ptr<JPH::PhysicsSystem> system;
    uint32_t addedSinceOptimize = 0;
    physics_detail::LayerTable layers;
    std::vector<Joint*> joints; ///< attached ones
    std::vector<Joint*> jointScratch; ///< step()'s copy of `joints`
    /// Surfaces: names interned; bodies without a component to hold one (createStaticBody) by id.
    std::vector<std::string> surfaceNames{""};
    std::unordered_map<uint32_t, uint16_t> staticSurfaces;
    uint16_t surfaceId(const std::string& name)
    {
        if (name.empty()) return 0;
        for (size_t i = 1; i < surfaceNames.size(); ++i)
            if (surfaceNames[i] == name) return uint16_t(i);
        surfaceNames.push_back(name);
        return uint16_t(surfaceNames.size() - 1);
    }

    /// Contact pairs (body level; Jolt reports sub-shape pairs, so count them).
    physics_detail::ContactQueue contacts;
    struct Pair {
        int count = 0;
        bool sensor = false;
        bool persisted = false;
        bool enteredNow = false; ///< began in this step's events (no "stay" for it yet)
        glm::vec3 point{0.0f}, normal{0.0f};
    };
    std::unordered_map<uint64_t, Pair> pairs;
    std::vector<physics_detail::ContactQueue::Event> scratch;
    /// What dispatchContacts reports this step (kept: no allocation per step).
    enum class Phase { Enter, Stay, Exit };
    struct ContactOut {
        Phase phase;
        bool sensor;
        JPH::BodyID a, b;
        glm::vec3 point, normal;
        float speed;
    };
    std::vector<ContactOut> contactOut;

    /// drawDebug: triangles of each shape in its centre-of-mass space (holding a
    /// reference to the shape, so its address can't be reused). Cleared in shutdown.
    struct ShapeTriangles {
        JPH::RefConst<JPH::Shape> keep;
        std::vector<glm::vec3> tris;
    };
    std::unordered_map<const JPH::Shape*, ShapeTriangles> debugTriangles;

    void dispatchContacts();
};

namespace physics_detail {

inline JPH::BodyInterface& bodies()
{
    return Engine::get().physics().impl().system->GetBodyInterface();
}

inline bool physicsAlive()
{
    return g_joltInitialized && Engine::exists() && Engine::get().physics().impl().system;
}

/// Body user data, tagged in the low 2 bits (pointers are 8-byte aligned):
///   0: Rigidbody*   1: (value << 2) for GameObject-less static bodies
///   2: CharacterController* (its inner body)
inline JPH::uint64 staticUserData(uint32_t value)
{
    return (JPH::uint64(value) << 2) | 1u;
}

inline JPH::uint64 characterUserData(const CharacterController* c)
{
    return reinterpret_cast<JPH::uint64>(c) | 2u;
}

struct BodyOwner {
    GameObject* gameObject = nullptr;
    Rigidbody* body = nullptr;
    uint32_t userValue = 0;
};

/// Defined in PhysicsWorld.cpp.
BodyOwner ownerOf(JPH::uint64 userData);
std::string surfaceOf(const JPH::Body& body);
/// A collision layer name -> its index (warns about unknown ones, which become Default).
int resolveLayer(const std::string& name, const std::string& who);

/// Defined in Collider.cpp: the collider's shape with a world scale baked in (Jolt ScaledShape).
JPH::RefConst<JPH::Shape> scaledShape(const Collider& collider, const glm::vec3& scale);

} // namespace physics_detail

} // namespace ve
