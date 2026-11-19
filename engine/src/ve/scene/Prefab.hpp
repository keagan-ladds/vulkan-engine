#pragma once

// =============================================================================
// Prefabs - an object (its type, fields, persistent components and children)
// saved as a reusable asset file.
//
//   ve::prefab::save(hut, "prefabs/hut.prefab");                  // the editor: Edit > Save as prefab
//   ve::GameObject* h = ve::prefab::instantiate(scene, "prefabs/hut.prefab", pos, rot);
//
// A .prefab file is JSON: {"version": 1, "object": <the object as a scene file
// stores it, at the origin>}. Instances are ordinary objects with fresh ids
// (children's ids follow from the root's, so a save game can recreate them
// with the same ids). A PrefabInstance component on the root remembers the
// source: save games recreate instances spawned at runtime from it (player
// buildings), and the editor shows it. Instances are copies: editing the
// prefab file later doesn't change existing instances.
// =============================================================================

#include "ve/scene/Components.hpp"
#include "ve/scene/Reflection.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>

namespace ve {

class Scene;

class PrefabInstance : public Component {
public:
    std::string prefab;      // asset path of the source
    bool saveInGame = true;  // runtime-spawned instances are recreated by save games
};

namespace prefab {

// The object (+ persistent children/components) at the origin, as prefab JSON.
Json toJson(const GameObject& object);
bool save(const GameObject& object, const std::string& path);
// The prefab's JSON (cached after the first load); null if it can't be read.
const Json* load(const std::string& path);
void clearCache();

// A new instance. id: the root's id (0 = a fresh one); children get ids derived from it.
GameObject* instantiate(Scene& scene, const std::string& path, const glm::vec3& position = glm::vec3(0.0f),
                        const glm::quat& rotation = glm::quat(1, 0, 0, 0), GameObject* parent = nullptr, uint64_t id = 0);
// Same, from prefab JSON already in hand (e.g. the editor's undo snapshots).
GameObject* instantiateJson(Scene& scene, const Json& prefabJson, const std::string& path, const glm::vec3& position,
                            const glm::quat& rotation, GameObject* parent = nullptr, uint64_t id = 0);

} // namespace prefab
} // namespace ve
