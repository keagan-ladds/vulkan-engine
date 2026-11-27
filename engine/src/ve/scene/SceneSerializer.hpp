#pragma once

// =============================================================================
// SceneSerializer - persistent GameObjects <-> JSON scene files.
//
//   ve::SceneSerializer::load(scene, "scenes/savanna.json");   // e.g. at startup
//   ve::SceneSerializer::save(scene, "scenes/savanna.json");   // the editor does this
//
//   // Behind a loading screen, without freezing it (the usual way:
//   // Engine::addSceneLoad does this as a load step):
//   auto load = ve::SceneSerializer::loadAsync(scene, "scenes/level1.json");
//   while (!load->done()) { load->step(8.0f); /* ...draw a frame... */ }
//
// Only objects with GameObject::persistent are saved (with their persistent
// children nested). Code-spawned objects stay out of the file. Each object
// stores its registered type, name, active flag, tag, local transform, its
// type's reflected fields and its persistent components (type + fields).
// Format: docs/05-editor.md. Paths are resolved like other assets (search
// paths; the source assets folder first when VE_SOURCE_ASSETS_DIR is set).
// =============================================================================

#include "ve/core/Jobs.hpp"
#include "ve/scene/Reflection.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ve {

/// A scene file loading over several frames (SceneSerializer::loadAsync).
/// Reading and parsing the JSON runs on the job pool; creating the objects is
/// main-thread work done a time slice per step(). The created objects don't
/// start until the whole file is in (Scene::holdStarts), so they start together,
/// like after load(); code right after the step() that finished can still
/// adjust them before they start. Dropping the SceneLoad early stops it (what
/// was created stays and starts).
class SceneLoad {
public:
    /// Starts reading and parsing `path` (an asset path) on the job pool; use SceneSerializer::loadAsync.
    SceneLoad(Scene& scene, std::filesystem::path path);
    ~SceneLoad();
    SceneLoad(const SceneLoad&) = delete;
    SceneLoad& operator=(const SceneLoad&) = delete;

    /// Main thread, once per frame: creates objects for up to budgetMs
    /// (<= 0: everything at once). Returns progress() afterwards.
    float step(float budgetMs);
    /// 0..1: parsing counts for the first 10 %, the root objects for the rest.
    float progress() const;
    /// Every object is created (or the load failed: see ok()).
    bool done() const { return m_done; }
    /// done() and the file was read and parsed (objects of unknown types still
    /// load as plain GameObjects, with a warning).
    bool ok() const { return m_done && m_error.empty(); }
    /// Why the load failed ("" if it didn't).
    const std::string& error() const { return m_error; }
    /// The root objects created (complete once done()).
    const std::vector<GameObject*>& roots() const { return m_roots; }
    /// The scene file being loaded, as given.
    const std::filesystem::path& path() const { return m_path; }

private:
    struct Parsed {
        Json json;
        std::string error;
        std::filesystem::path file;
    };
    void finish();
    Scene* m_scene;
    std::filesystem::path m_path;
    std::shared_ptr<Parsed> m_parsed;
    jobs::Handle m_job;
    size_t m_next = 0, m_total = 0;
    bool m_holding = false, m_done = false;
    std::vector<GameObject*> m_roots;
    std::string m_error;
};

/// Reads and writes scene files (JSON): the persistent objects of a scene, with
/// their reflected fields and components. All static; main thread only.
class SceneSerializer {
public:
    /// One object (and its persistent children). worldTransform: store the world
    /// transform (for roots whose parent isn't saved).
    static Json serializeObject(const GameObject& object, bool worldTransform = false);
    /// All persistent objects whose parent chain isn't persistent.
    static Json serializeScene(const Scene& scene);

    /// Creates an object (+children) from JSON. newIds: give it fresh ids (duplicate/paste).
    static GameObject* createObject(Scene& scene, const Json& json, GameObject* parent = nullptr, bool newIds = false);
    /// Applies JSON to an existing object in place (name, active, tag, transform,
    /// fields, persistent components' fields). Returns false if the persistent
    /// component list differs (then re-create the object instead).
    static bool applyObject(GameObject& object, const Json& json);
    /// Destroys every persistent root object (e.g. before loading another scene).
    static void clearPersistent(Scene& scene);

    /// A component as JSON: {type, enabled, fields}.
    static Json serializeComponent(const Component& component);
    /// The reflected fields of `object` (an instance of `type`) as a JSON object.
    static Json serializeFields(const TypeInfo& type, const void* object);
    /// Sets the fields of `object` that `fields` (a JSON object) lists; others keep their values.
    static void applyFields(const TypeInfo& type, void* object, const Json& fields);

    /// Writes the scene's persistent objects to `path` (relative = under writablePath()).
    /// @return false (with an error logged) if the file can't be written.
    static bool save(const Scene& scene, const std::filesystem::path& path);
    /// Loads a scene file into the scene (adds to what's there). Returns the roots.
    static std::vector<GameObject*> load(Scene& scene, const std::filesystem::path& path);
    /// Creates the objects a parsed scene file lists (what load() does after reading). Returns the roots.
    static std::vector<GameObject*> loadJson(Scene& scene, const Json& json);
    /// Starts loading a scene file in the background (see SceneLoad); call
    /// step() every frame until done(). The file is read and parsed right away
    /// on the job pool.
    static std::shared_ptr<SceneLoad> loadAsync(Scene& scene, const std::filesystem::path& path);

    /// Where the editor saves: the source assets folder if known, else the first search path.
    static std::filesystem::path writablePath(const std::filesystem::path& relative);
    /// The scene file last loaded or saved (the editor's "current scene").
    static const std::filesystem::path& lastPath() { return s_lastPath; }

    /// An object id as the hex string scene files store.
    static std::string idToString(uint64_t id);
    /// Parses an id written by idToString (0 if it isn't one).
    static uint64_t idFromString(const std::string& s);

private:
    friend class SceneLoad;
    static void markLevelObjects(Scene& scene, const std::vector<GameObject*>& roots);
    static inline std::filesystem::path s_lastPath;
};

} // namespace ve
