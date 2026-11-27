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
// Builder: the build mode. B opens the build menu (categories of piece
// cards with baked 3D icons); pick one and its ghost follows the aim, snaps
// to other pieces' points and turns red where it doesn't fit; place with the
// left mouse button, remove a piece with the right one, B for the menu
// again, Escape to stop building. The pieces are prefab instances spawned
// while playing, so save games keep them.

#include <ve/scene/Reflection.hpp>
#include <ve/gameplay/Placement.hpp>
#include <ve/scene/GameObject.hpp>
#include <ve/scene/ObjectRef.hpp>
#include <ve/ui/Gui.hpp>
#include <ve/scene/SaveGame.hpp>

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <vector>

namespace savanna {

class BuildPiece : public ve::GameObject {
public:
    // (Appended only: prefabs store the index.) Door / Window are fittings: they
    // snap into a WallDoor / WallWindow opening, turning to match it.
    // Generator / LampPost / FuelBarrel: power (Power.hpp).
    enum class Kind { Foundation, Floor, Wall, WallDoor, WallWindow, Railing, Post, Beam, RoofMetal, RoofThatch, Stairs, Fence, Door, Window,
                      Generator, LampPost, FuelBarrel };
    enum class Material { Mud, Plaster, Timber, Stone };

    BuildPiece() : GameObject("Piece") {}
    void start() override;

    Kind kind = Kind::Wall;
    Material material = Material::Mud;

    static constexpr float kTile = 2.0f;        // floor tile / wall width
    static constexpr float kWall = 2.5f;        // wall height
    static constexpr float kFoundation = 0.5f;  // foundation top above its origin

    static BuildPiece* of(ve::GameObject* object); // the piece an object belongs to (itself or a parent)
    bool isFitting() const { return kind == Kind::Door || kind == Kind::Window; }
};

// A door's hinge (on the Door piece): Use opens / closes it, the leaf swings
// over a moment. Its state is part of save games.
class DoorHinge : public ve::Component, public ve::Saveable {
public:
    float openAngle = 100.0f; // degrees
    float speed = 3.0f;       // 1 / seconds for a full swing
    bool open() const { return m_open; }
    void setOpen(bool open) { m_open = open; }
    void setLeaf(ve::GameObject* leaf) { m_leaf = leaf; }

    void update(float dt) override;
    void saveState(ve::Json& out) const override { out = {{"open", m_open}}; }
    void loadState(const ve::Json& in) override;

private:
    ve::ObjectRef<> m_leaf;
    bool m_open = false;
    float m_t = 0.0f; // 0 closed .. 1 open
};

class Builder : public ve::GameObject {
public:
    Builder();
    void start() override;
    void update(float dt) override;
    void lateUpdate(float dt) override;
    void onDestroy() override;

    // The build menu (B): pick a piece from categories of cards.
    void openMenu();
    void closeMenu();
    bool menuOpen() const;
    ve::ui::Canvas& menu() { return *m_menu; }
    void showCategory(const std::string& category);
    void bakeIcons(); // the cards' pictures (at load)

    struct Entry {
        std::string name, prefab, category, description;
        std::shared_ptr<ve::Texture> icon; // baked from the piece's mesh
    };
    const std::vector<Entry>& catalogue() const { return m_catalogue; }
    bool active() const { return m_active; }
    void setActive(bool on);
    bool justStopped() const; // Escape stopped building this frame (it isn't also a pause)
    int selection() const { return m_index; }
    void select(int index);
    void setYawDegrees(float yaw) { m_yaw = glm::radians(yaw); }
    static bool building(const ve::Scene& scene); // a Builder in build mode (other tools step aside)

    // Tests: aim here instead of where the camera looks; place() = the Place action.
    void setAimOverride(std::optional<glm::vec3> aim) { m_aimOverride = aim; }
    ve::GameObject* place();             // the placed piece, or null (invalid / not ready)
    // A placed piece (or part of one), and the fittings in its openings.
    // Returns how many pieces went (0 = nothing removable there).
    int remove(ve::GameObject* piece);
    glm::quat previewRotation() const { return m_rot; }
    bool previewValid() const { return m_preview.valid(); }
    bool previewSnapped() const { return m_snapped; }
    bool previewNeedsPower() const { return m_noPower; } // blocked: no generator reaches the spot
    glm::vec3 previewPosition() const { return m_pos; }

private:
    void loadCatalogue();
    void updatePreview();
    void rebuildNavigation(const glm::vec3& at);
    void registerRequests();                   // the host's side of building (ve::net)
    int removeNow(ve::GameObject* piece);      // (host)
    ve::ObjectRef<> m_placed;                  // what the last request placed (host)
    int m_removed = 0;
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
    bool m_noPower = false; // a lamp outside every generator's reach
    std::optional<glm::vec3> m_aimOverride;
    // UI.
    void buildMenu();
    void buildStatus();
    ve::ui::Canvas* m_menu = nullptr;    // the build menu (modal)
    ve::ui::Canvas* m_status = nullptr;  // the panel while placing
    ve::ui::Grid* m_cards = nullptr;
    ve::ui::Label* m_cardInfo = nullptr;
    std::string m_category;
    uint64_t m_stoppedFrame = ~0ull;
};

} // namespace savanna
