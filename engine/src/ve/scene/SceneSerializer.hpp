#pragma once

// =============================================================================
// SceneSerializer - persistent GameObjects <-> JSON scene files.
//
//   ve::SceneSerializer::load(scene, "scenes/savanna.json");   // e.g. at startup
//   ve::SceneSerializer::save(scene, "scenes/savanna.json");   // the editor does this
//
// Only objects with GameObject::persistent are saved (with their persistent
// children nested). Code-spawned objects stay out of the file. Each object
// stores its registered type, name, active flag, tag, local transform, its
// type's reflected fields and its persistent components (type + fields).
// Format: docs/05-editor.md. Paths are resolved like other assets (search
// paths; the source assets folder first when VE_SOURCE_ASSETS_DIR is set).
// =============================================================================

#include "ve/scene/Reflection.hpp"

#include <filesystem>
#include <vector>

namespace ve {

class SceneSerializer {
public:
    // One object (and its persistent children). worldTransform: store the world
    // transform (for roots whose parent isn't saved).
    static Json serializeObject(const GameObject& object, bool worldTransform = false);
    // All persistent objects whose parent chain isn't persistent.
    static Json serializeScene(const Scene& scene);

    // Creates an object (+children) from JSON. newIds: give it fresh ids (duplicate/paste).
    static GameObject* createObject(Scene& scene, const Json& json, GameObject* parent = nullptr, bool newIds = false);
    // Applies JSON to an existing object in place (name, active, tag, transform,
    // fields, persistent components' fields). Returns false if the persistent
    // component list differs (then re-create the object instead).
    static bool applyObject(GameObject& object, const Json& json);
    // Destroys every persistent root object (e.g. before loading another scene).
    static void clearPersistent(Scene& scene);

    static Json serializeComponent(const Component& component);
    static Json serializeFields(const TypeInfo& type, const void* object);
    static void applyFields(const TypeInfo& type, void* object, const Json& fields);

    static bool save(const Scene& scene, const std::filesystem::path& path);
    // Loads a scene file into the scene (adds to what's there). Returns the roots.
    static std::vector<GameObject*> load(Scene& scene, const std::filesystem::path& path);
    static std::vector<GameObject*> loadJson(Scene& scene, const Json& json);

    // Where the editor saves: the source assets folder if known, else the first search path.
    static std::filesystem::path writablePath(const std::filesystem::path& relative);
    static const std::filesystem::path& lastPath() { return s_lastPath; }

    static std::string idToString(uint64_t id);
    static uint64_t idFromString(const std::string& s);

private:
    static inline std::filesystem::path s_lastPath;
};

} // namespace ve
