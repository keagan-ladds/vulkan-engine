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
#include "ve/scene/Reflection.hpp"

#include <filesystem>
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

    ObjectState snapshot(uint64_t id) const;
    void restoreSnapshot(const Json& sceneJson);
    void applyStates(const std::vector<ObjectState>& states);
    void pushUndo(UndoEntry entry);
    void beginEdit(GameObject& object);
    void endEdit(const std::string& label);
    void afterTransformEdit(GameObject& object);

    void updateCamera(float dt);
    void handleShortcuts();
    void drawMenuBar();
    void drawHierarchy();
    void drawHierarchyNode(GameObject& object);
    void drawInspector();
    bool drawField(const FieldInfo& field, void* object, GameObject& owner);
    void drawViewport();
    void drawPlayToolbar();
    std::vector<std::string> listAssets(const std::string& extensions);

    Engine* m_engine = nullptr;
    Mode m_mode = Mode::Off;
    Mode m_pendingMode = Mode::Off;
    bool m_hasPending = false;
    int m_frames = 0;
    GameObject* m_camera = nullptr;
    float m_camYaw = 0.0f, m_camPitch = -0.3f, m_camSpeed = 12.0f;
    bool m_looking = false;
    uint64_t m_selected = 0;
    PickingPass* m_picking = nullptr;
    glm::vec2 m_pressPos{0.0f};
    bool m_pressInViewport = false;

    Gizmo m_gizmo;
    Gizmo::Options m_gizmoOptions;
    bool m_showCodeObjects = true;
    bool m_showIcons = true;
    char m_filter[64] = {};

    Json m_playSnapshot;
    std::vector<UndoEntry> m_undo, m_redo;
    ObjectState m_editBefore;
    bool m_editing = false;
    glm::vec3 m_eulerCache{0.0f};
    glm::quat m_eulerFor{0, 0, 0, 0};
    uint64_t m_eulerObject = 0;

    std::string m_scenePath;
    char m_saveAsBuffer[256] = "scenes/untitled.json";
    bool m_openSaveAs = false;
    std::vector<std::string> m_assetCache;
    std::string m_assetCacheKey;
    float m_assetCacheAge = 1e9f;
    std::string m_status;
    float m_statusTime = 0.0f;
};

} // namespace ve
