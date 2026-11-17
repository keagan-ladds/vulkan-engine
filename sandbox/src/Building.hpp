#pragma once

// Building (game side, on the engine's prefabs + ve::SnapPoint / placement).
//
// BuildPiece: one modular piece of the kit (foundation, floor, walls with
// doors / windows, posts, beams, roofs, stairs, railings, fences). Its kind
// and material are fields, so each piece is a tiny prefab
// (assets/prefabs/building/*.prefab); the mesh, colliders and snap points are
// made here. Grid: 2 m tiles, 2.5 m walls, foundations 0.5 m high, 45 deg
// rotations (the reference sheet's numbers).
//
// Builder: the build mode (B). A ghost of the selected piece follows the
// aim, snaps to other pieces' points, turns red where it doesn't fit; place
// with the left mouse button, remove a piece with the right one. The pieces
// are prefab instances spawned while playing, so save games keep them.

#include <ve/scene/Reflection.hpp>
#include <ve/gameplay/Placement.hpp>
#include <ve/scene/GameObject.hpp>

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <vector>

namespace savanna {

class BuildPiece : public ve::GameObject {
public:
    enum class Kind { Foundation, Floor, Wall, WallDoor, WallWindow, Railing, Post, Beam, RoofMetal, RoofThatch, Stairs, Fence };
    enum class Material { Mud, Plaster, Timber, Stone };

    BuildPiece() : GameObject("Piece") {}
    void start() override;

    Kind kind = Kind::Wall;
    Material material = Material::Mud;

    static constexpr float kTile = 2.0f;        // floor tile / wall width
    static constexpr float kWall = 2.5f;        // wall height
    static constexpr float kFoundation = 0.5f;  // foundation top above its origin

    static BuildPiece* of(ve::GameObject* object); // the piece an object belongs to (itself or a parent)
};

class Builder : public ve::GameObject {
public:
    Builder();
    void start() override;
    void update(float dt) override;
    void lateUpdate(float dt) override;
    void onGui() override;
    void onDestroy() override;

    struct Entry {
        std::string name, prefab;
    };
    const std::vector<Entry>& catalogue() const { return m_catalogue; }
    bool active() const { return m_active; }
    void setActive(bool on);
    int selection() const { return m_index; }
    void select(int index);
    void setYawDegrees(float yaw) { m_yaw = glm::radians(yaw); }
    static bool building(const ve::Scene& scene); // a Builder in build mode (other tools step aside)

    // Tests: aim here instead of where the camera looks; place() = the Place action.
    void setAimOverride(std::optional<glm::vec3> aim) { m_aimOverride = aim; }
    ve::GameObject* place();             // the placed piece, or null (invalid / not ready)
    bool remove(ve::GameObject* piece);  // a placed piece (or part of one)
    bool previewValid() const { return m_preview.valid(); }
    bool previewSnapped() const { return m_snapped; }
    glm::vec3 previewPosition() const { return m_pos; }

private:
    void loadCatalogue();
    void updatePreview();
    void rebuildNavigation(const glm::vec3& at);
    ve::GameObject* aimedPiece() const;

    std::vector<Entry> m_catalogue;
    ve::PlacementPreview m_preview;
    bool m_active = false;
    int m_index = 0;
    float m_yaw = 0.0f;
    glm::vec3 m_pos{0.0f};
    glm::quat m_rot{1, 0, 0, 0};
    bool m_snapped = false;
    bool m_aimHit = false;
    std::optional<glm::vec3> m_aimOverride;
};

} // namespace savanna
