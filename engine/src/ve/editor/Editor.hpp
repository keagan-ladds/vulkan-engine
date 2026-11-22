#pragma once

// =============================================================================
// Editor - the in-engine scene editor (F2). See docs/05-editor.md.
//
// Modes: Off (the game), Edit (game paused; hierarchy, inspector, gizmo,
// picking, editor camera), Play (the game runs inside the editor; Stop
// restores every persistent object from the snapshot taken at Play).
//
// Games don't need to do anything to get it (EngineConfig::enableEditor).
// To make your own GameObject classes / components creatable and saved,
// register them with VE_REGISTER_TYPE (ve/scene/Reflection.hpp) and load
// your scene file at startup (ve::SceneSerializer::load).
// =============================================================================

#include "ve/editor/Gizmo.hpp"
#include "ve/world/Terrain.hpp"
#include "ve/world/Vegetation.hpp"
#include "ve/scene/ObjectRef.hpp"
#include "ve/scene/Reflection.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ve {

class Engine;
class GameObject;
class Component;
class PickingPass;
struct FieldInfo;

class Editor {
public:
    enum class Mode { Off, Edit, Play };

    void init(Engine& engine);
    void shutdown();

    // Engine hooks (every frame).
    void beginFrame(float realDt); // shortcuts, editor camera, pick results
    void onGui();                  // panels, gizmo, debug shapes
    bool paused() const { return m_mode == Mode::Edit; }
    // In Edit mode, or about to be (a game that starts in the editor): loading
    // then shows the editor's progress bar instead of the game's loading screen.
    bool editing() const { return m_mode == Mode::Edit || (m_hasPending && m_pendingMode == Mode::Edit); }

    Mode mode() const { return m_mode; }
    void setMode(Mode mode);

    GameObject* selected() const;
    void select(GameObject* object);
    // Picks the object under a framebuffer pixel (result arrives a few frames later).
    void requestPick(glm::ivec2 pixel);

    // Undo-able structural operations (also used by the menus).
    GameObject* createObject(const Json& json, GameObject* parent, const std::string& label);
    void deleteObject(GameObject& object);
    GameObject* duplicate(GameObject& object);
    void undo();
    void redo();

    bool saveScene(const std::filesystem::path& path);
    void openScene(const std::filesystem::path& path);
    void newScene();

private:
    // A snapshot of one object subtree (null = absent) for undo.
    struct ObjectState {
        uint64_t id = 0;
        uint64_t parent = 0;
        Json json; // null: the object doesn't exist in this state
    };
    struct UndoEntry {
        std::string label;
        std::vector<ObjectState> before, after;
    };

    // The member functions are defined in several files, one per part of the
    // editor (shared private helpers: EditorInternal.hpp).

    // Editor.cpp: modes, shortcuts, menu bar, scene files.
    void handleShortcuts();
    void drawMenuBar();
    void drawLoading();
    void drawPlayToolbar();
    std::vector<std::string> listAssets(const std::string& extensions);

    // EditorUndo.cpp: snapshots and undo / redo.
    ObjectState snapshot(uint64_t id) const;
    void restoreSnapshot(const Json& sceneJson);
    void applyStates(const std::vector<ObjectState>& states);
    void pushUndo(UndoEntry entry);
    void beginEdit(GameObject& object);
    void endEdit(const std::string& label);
    // Re-takes the cached m_editBefore of the selection when stale (see m_editBefore).
    void refreshEditBefore(const GameObject& selection, float dt);
    void afterTransformEdit(GameObject& object);

    // EditorHierarchy.cpp: the hierarchy panel and the Create menu.
    void drawHierarchy();
    void drawHierarchyNode(GameObject& object);
    void drawCreateMenu();

    // EditorInspector.cpp: the inspector panel.
    void drawInspector();
    // owner: the object being edited (undo); null for asset files (no undo: Revert).
    bool drawField(const FieldInfo& field, void* object, GameObject* owner);

    // EditorViewport.cpp: editor camera, icons, picking, gizmo, terrain tools.
    GameObject* camera() const; // the editor camera, null if gone (e.g. the scene was cleared)
    GameObject& ensureCamera(); // spawns it if needed
    void updateCamera(float dt);
    void drawViewport();
    void drawTerrainTools(Terrain& terrain);

    // EditorAssets.cpp: the terrain's biome list and the asset editor (.biome,
    // .foliage, .grass files: edited live through DataAssets, saved on demand).
    void drawTerrainBiomes(Terrain& terrain);
    void drawAssetsMenu();
    void drawAssetEditor();
    void drawNewAssetPopup();
    void openAsset(const std::string& path, bool keepHistory = false);
    void loadAssetCopy();
    // A biome's foliage / grass list. owner: the terrain (in-scene biome: undo), null for a .biome file.
    // `openNext` gets a plant file to open after the panel is drawn.
    bool drawPlantList(const char* label, std::vector<BiomePlant>& list, bool grass, std::string& openNext, GameObject* owner);
    void commitAssetCopy();
    // Opens the "new asset" popup; onCreated runs with the new file's path.
    void newAssetDialog(const std::string& path, Json content, std::function<void(const std::string&)> onCreated);
    // Terrain brush in the viewport (true = the tool owns the mouse this frame).
    bool terrainBrush(Terrain& terrain, const ViewProjector& view, DebugDraw& dd, glm::vec2 mouse);

    Engine* m_engine = nullptr;
    Mode m_mode = Mode::Off;
    Mode m_pendingMode = Mode::Off;
    bool m_hasPending = false;
    int m_frames = 0;
    std::vector<Component*> m_inspectScratch; // the selection's components while drawing the inspector
    // By id, not pointer: the game may clear the scene (back to its menu) and
    // take the camera object with it (docs/04-agent-handoff.md, gotchas).
    ObjectRef<> m_camera;
    glm::vec3 m_camPosition{0.0f}; // where a re-spawned camera starts
    float m_camYaw = 0.0f, m_camPitch = -0.3f, m_camSpeed = 12.0f;
    bool m_looking = false;
    ObjectRef<> m_selected;
    PickingPass* m_picking = nullptr;
    glm::vec2 m_pressPos{0.0f};
    bool m_pressInViewport = false;

    int m_terrainTool = -1; // Terrain::Brush::Mode, -1 = off
    Terrain::Brush m_brush;
    bool m_brushing = false;

    Gizmo m_gizmo;
    Gizmo::Options m_gizmoOptions;
    bool m_showCodeObjects = true;
    bool m_showIcons = true;
    // Viewport icons, kept between frames to reuse their memory.
    struct IconCandidate {
        GameObject* object;
        int rank; // lower wins when an object has several
        const char* letter;
        uint32_t color;
    };
    struct Icon {
        glm::vec2 pos;
        uint64_t id;
    };
    std::vector<IconCandidate> m_iconCandidates;
    std::vector<Icon> m_icons;
    char m_filter[64] = {};

    Json m_playSnapshot;
    std::vector<UndoEntry> m_undo, m_redo;
    // The selection's "before" state for one-frame edits (checkbox / combo,
    // see drawField). Serializing the subtree is costly, so it's cached and
    // re-taken only when stale: another selection, any undo-able change, undo /
    // redo, or older than a second (runInEditor objects may change themselves).
    ObjectState m_editBefore;
    bool m_editBeforeValid = false;
    float m_editBeforeAge = 0.0f;
    bool m_editing = false;
    glm::vec3 m_eulerCache{0.0f};
    glm::quat m_eulerFor{0, 0, 0, 0};
    ObjectRef<> m_eulerObject; // (only compared, never looked up)

    std::string m_scenePath;
    char m_saveAsBuffer[256] = "scenes/untitled.json";
    bool m_openSaveAs = false;
    char m_prefabBuffer[256] = "prefabs/object.prefab";
    bool m_openSavePrefab = false;
    bool m_snapToPoints = true; // moving an object with SnapPoints snaps it to others nearby
    std::map<std::string, std::vector<std::string>> m_assetLists; // listAssets() by extensions
    float m_assetCacheAge = 1e9f; // all lists are re-scanned after 3 s (1e9 = now)
    std::string m_status;
    float m_statusTime = 0.0f;

    // Asset editor (EditorAssets.cpp): an editable copy of one data file.
    std::string m_assetPath; // "" = closed
    std::vector<std::string> m_assetHistory; // Back
    uint64_t m_assetSeen = 0; // DataAssets::generation() the copy matches
    TerrainBiome m_assetBiome;
    std::unique_ptr<FoliageType> m_assetFoliage;
    std::unique_ptr<GrassLayer> m_assetGrass;
    bool m_openNewAsset = false;
    char m_newAssetBuffer[256] = {};
    std::string m_newAssetExt; // the extension the new file must keep
    Json m_newAssetContent;
    std::function<void(const std::string&)> m_newAssetDone;
};

} // namespace ve
