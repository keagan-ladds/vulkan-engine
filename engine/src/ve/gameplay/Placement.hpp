#pragma once

// =============================================================================
// Placement - snapping pieces together and previewing where they'll go
// (building systems, modular level kits in the editor).
//
// The engine provides the mechanics; the game decides what the pieces are,
// which connect, what they cost and how the build tool feels.
//
//   // On a piece (usually in its prefab, or added in code): where it connects.
//   auto& sp = wall.addComponent<ve::SnapPoint>();
//   sp.offset = {1.0f, 0.0f, 0.0f};                       // local position
//
//   // While placing: the preview follows the aim and snaps.
//   ve::PlacementPreview preview;
//   preview.show(scene, "prefabs/wall.prefab");
//   glm::quat rot = glm::angleAxis(ve::placement::snapAngle(yaw, glm::radians(45.0f)), up);
//   glm::vec3 pos = aim;
//   if (auto s = ve::placement::findSnap(scene, preview.snapPoints(), rot, aim, 0.75f)) pos = s->position;
//   preview.setPose(pos, rot);
//   preview.setValid(ve::placement::fits(physics(), preview.volume(), pos, rot));
//   if (input().actionPressed("Place") && preview.valid()) ve::prefab::instantiate(scene, path, pos, rot);
//
// Snap points are just points with a type: a piece keeps the rotation the
// player chose and moves so that one of its points lands on a nearby point of
// another piece (whichever pair is closest to where the player aims). Types
// say what may connect ("accepts", comma-separated, "*" = anything).
// =============================================================================

#include "ve/render/Geometry.hpp"
#include "ve/scene/Components.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ve {

class PhysicsWorld;
class Scene;
class StandardMaterial;
struct RaycastHit;

class SnapPoint : public Component {
public:
    glm::vec3 offset{0.0f};       // in the object's local space
    std::string type = "grid";    // what this point is
    std::string accepts = "grid"; // types it connects to (comma-separated, "*" = any)

    glm::vec3 worldPosition() const;
    bool connectsTo(const std::string& otherType, const std::string& otherAccepts) const; // either side accepts the other
};

// On a preview's root: the object is a ghost (no physics, not saved). Pieces
// that create colliders in start() should skip them when
// PlacementGhost::is(*this); prefab-declared Rigidbodies, SnapPoints and
// Interactables are disabled for them (snap points the piece creates itself
// are read by the preview and ignored by findSnap).
class PlacementGhost : public Component {
public:
    static bool is(const GameObject& object); // the object or one of its parents is a ghost
};

// Part of a piece's solid volume for the fit test (in the object's local
// space). Pieces whose bounding box is a poor fit (a sloped roof over the
// wall it rests on, an L-shaped piece) list their real volume as boxes;
// without any, the fit test uses the bounds of what the piece draws.
class PlacementBox : public Component {
public:
    glm::vec3 center{0.0f};
    glm::vec3 halfExtents{0.5f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

namespace placement {

// A box relative to a piece's origin (unrotated).
struct OrientedBox {
    glm::vec3 center{0.0f};
    glm::vec3 half{0.5f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

// A snap point relative to a piece's origin (unrotated).
struct LocalSnap {
    glm::vec3 offset;
    std::string type, accepts;
};
struct SnapResult {
    glm::vec3 position;            // where the piece's origin goes
    glm::vec3 point;               // the joint, world
    const SnapPoint* target;       // the scene point it snapped to
    float distance;                // how far the snap moved the piece
};

// Every enabled SnapPoint in the object's subtree, relative to its root (unrotated, unscaled origin).
std::vector<LocalSnap> snapPointsOf(const GameObject& root);
// A piece (its points, rotated by `rotation`) aimed at `aim`: the placement that
// puts one of its points on a scene point, choosing the pair closest together
// when the piece's origin is at `aim`, within `radius`. Points on `ignore`
// (and its children) and on ghosts are skipped.
std::optional<SnapResult> findSnap(const Scene& scene, const std::vector<LocalSnap>& points, const glm::quat& rotation,
                                   const glm::vec3& aim, float radius, const GameObject* ignore = nullptr);

// Grid helpers: round to the nearest multiple of `cell` (per axis; 0 = leave that axis).
glm::vec3 snapToGrid(const glm::vec3& p, const glm::vec3& cell, const glm::vec3& origin = glm::vec3(0.0f));
float snapAngle(float radians, float step);

// The local bounds of everything the object's subtree draws (relative to its root).
AABB localBounds(const GameObject& root);
// The piece's volume: its PlacementBoxes, or else its drawn bounds as one box.
std::vector<OrientedBox> volumeOf(const GameObject& root);
// Is the volume (placed at position / rotation, each box shrunk by `margin` on
// every side so neighbours that only touch are fine) free of physics bodies?
// `ignoreHit` lets the game allow some (e.g. the terrain, so foundations can sink in).
bool fits(const PhysicsWorld& physics, const std::vector<OrientedBox>& volume, const glm::vec3& position, const glm::quat& rotation,
          float margin = 0.08f, const std::function<bool(const RaycastHit&)>& ignoreHit = {});

} // namespace placement

// A see-through copy of a prefab that follows the aim: no physics, no snap
// points of its own for others, not saved, drawn tinted valid / invalid.
class PlacementPreview {
public:
    PlacementPreview();
    ~PlacementPreview();
    PlacementPreview(const PlacementPreview&) = delete;
    PlacementPreview& operator=(const PlacementPreview&) = delete;

    bool show(Scene& scene, const std::string& prefabPath); // (re)creates it if the prefab changed
    void hide();
    bool visible() const;
    const std::string& prefab() const { return m_prefab; }

    void setPose(const glm::vec3& position, const glm::quat& rotation);
    void setValid(bool valid);
    bool valid() const { return m_valid; }
    // Call once per frame (late): keeps the ghost look on renderers the piece creates over time.
    void update();

    GameObject* object() const;
    const std::vector<placement::LocalSnap>& snapPoints() const { return m_snaps; }
    AABB localBounds() const { return m_bounds; }
    const std::vector<placement::OrientedBox>& volume() const { return m_volume; }
    bool ready() const { return m_measured; } // the piece's start() ran: points and volume are known

    glm::vec4 validColor{0.35f, 0.95f, 0.5f, 0.38f};
    glm::vec4 invalidColor{1.0f, 0.3f, 0.25f, 0.42f};

private:
    Scene* m_scene = nullptr;
    uint64_t m_id = 0;
    std::string m_prefab;
    std::vector<placement::LocalSnap> m_snaps;
    std::vector<placement::OrientedBox> m_volume;
    AABB m_bounds{};
    bool m_valid = true;
    bool m_measured = false; // snap points / bounds read (after the piece's start())
    std::shared_ptr<StandardMaterial> m_material;
};

} // namespace ve
