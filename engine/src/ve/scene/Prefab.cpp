#include "ve/scene/Prefab.hpp"

#include "ve/assets/AssetManager.hpp"
#include "ve/core/Engine.hpp"
#include "ve/core/FileSystem.hpp"
#include "ve/core/Log.hpp"
#include "ve/scene/Ids.hpp"
#include "ve/scene/Scene.hpp"
#include "ve/scene/SceneSerializer.hpp"

#include <fstream>
#include <unordered_map>

namespace ve::prefab {

namespace {

std::unordered_map<std::string, Json>& cache()
{
    static std::unordered_map<std::string, Json> c;
    return c;
}

uint64_t mix(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return x ? x : 1;
}

// Ids for the whole tree: the root's, then derived ones (deterministic, so a
// save game recreating the root with its id gets the same child ids).
void assignIds(Json& j, uint64_t rootId, uint64_t& counter, bool root)
{
    j["id"] = idToHex(root ? rootId : mix(rootId + 0x9e3779b97f4a7c15ull * ++counter));
    if (auto it = j.find("children"); it != j.end() && it->is_array())
        for (Json& c : *it) assignIds(c, rootId, counter, false);
}

uint64_t freshId(const Scene& scene)
{
    uint64_t id = newObjectId();
    while (scene.findById(id)) id = newObjectId();
    return id;
}

} // namespace

Json toJson(const GameObject& object)
{
    Json o = SceneSerializer::serializeObject(object);
    o["position"] = {0.0f, 0.0f, 0.0f};
    o["rotation"] = {0.0f, 0.0f, 0.0f, 1.0f};
    // The source link is added per instance, not stored in the prefab itself.
    if (auto it = o.find("components"); it != o.end() && it->is_array()) {
        Json kept = Json::array();
        for (Json& c : *it)
            if (c.value("type", std::string()) != "PrefabInstance") kept.push_back(std::move(c));
        if (kept.empty()) o.erase("components");
        else o["components"] = std::move(kept);
    }
    return Json{{"version", 1}, {"object", std::move(o)}};
}

bool save(const GameObject& object, const std::string& path)
{
    const std::filesystem::path file = SceneSerializer::writablePath(path);
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file);
    if (!out) {
        log::error("Prefab: can't write {}", file.string());
        return false;
    }
    Json j = toJson(object);
    out << j.dump(2) << '\n';
    cache()[path] = std::move(j);
    log::info("Prefab saved: {}", file.string());
    return true;
}

const Json* load(const std::string& path)
{
    if (auto it = cache().find(path); it != cache().end()) return &it->second;
    try {
        const std::unique_ptr<std::istream> in = vfs::open(Engine::get().assets().resolve(path));
        if (!in) throw std::runtime_error("can't read");
        Json j;
        *in >> j;
        if (!j.contains("object")) throw std::runtime_error("no \"object\"");
        return &(cache()[path] = std::move(j));
    } catch (const std::exception& e) {
        log::warnOnce("Prefab '" + path + "': " + e.what());
        return nullptr;
    }
}

void clearCache()
{
    cache().clear();
}

GameObject* instantiateJson(Scene& scene, const Json& prefabJson, const std::string& path, const glm::vec3& position,
                            const glm::quat& rotation, GameObject* parent, uint64_t id)
{
    const auto object = prefabJson.find("object");
    Json o = object != prefabJson.end() ? *object : Json::object(); // (one copy: edited below)
    o["position"] = {position.x, position.y, position.z};
    o["rotation"] = {rotation.x, rotation.y, rotation.z, rotation.w};
    uint64_t counter = 0;
    assignIds(o, id && !scene.findById(id) ? id : freshId(scene), counter, true);
    Json& comps = o["components"];
    if (!comps.is_array()) comps = Json::array();
    comps.push_back({{"type", "PrefabInstance"}, {"fields", {{"prefab", path}}}});
    return SceneSerializer::createObject(scene, o, parent, false);
}

GameObject* instantiate(Scene& scene, const std::string& path, const glm::vec3& position, const glm::quat& rotation,
                        GameObject* parent, uint64_t id)
{
    const Json* j = load(path);
    return j ? instantiateJson(scene, *j, path, position, rotation, parent, id) : nullptr;
}

} // namespace ve::prefab
