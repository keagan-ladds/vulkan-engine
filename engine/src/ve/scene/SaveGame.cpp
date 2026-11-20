#include "ve/scene/SaveGame.hpp"

#include "ve/core/Engine.hpp"
#include "ve/core/Log.hpp"
#include "ve/physics/Physics.hpp"
#include "ve/scene/GameObject.hpp"
#include "ve/scene/Ids.hpp"
#include "ve/scene/Prefab.hpp"
#include "ve/scene/Scene.hpp"

#include <SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <typeindex>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace ve::save {

namespace {

// Level objects are found by persistent id, code-spawned ones by name.
std::string objectKey(const GameObject& o)
{
    return o.persistent ? idToHex(o.id()) : "name:" + o.name();
}

std::string componentKey(const GameObject& o, const Component& c)
{
    const TypeInfo* t = TypeRegistry::get().find(std::type_index(typeid(c)));
    return objectKey(o) + "/" + (t ? t->name : std::string(typeid(c).name()));
}

std::filesystem::path slotPath(const std::string& slot)
{
    std::string safe;
    for (char ch : slot) safe += (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_') ? ch : '_';
    return folder() / ((safe.empty() ? std::string("save") : safe) + ".json");
}

} // namespace

std::filesystem::path folder()
{
    if (const char* d = std::getenv("VE_SAVE_DIR"); d && *d) return d;
    const EngineConfig& cfg = Engine::get().config();
    if (!cfg.saveFolder.empty()) return cfg.saveFolder;
    std::string app;
    for (char ch : cfg.title) app += std::isalnum(static_cast<unsigned char>(ch)) ? ch : '_';
    std::filesystem::path base;
    if (char* pref = SDL_GetPrefPath("vulkan-engine", app.c_str())) {
        base = pref;
        SDL_free(pref);
    } else {
        base = std::filesystem::current_path();
    }
    return base / "saves";
}

Json capture(Scene& scene)
{
    Json moved = Json::object(), state = Json::object(), removed = Json::array();
    std::unordered_map<std::string, int> seen; // warn about ambiguous keys once
    for (const auto& up : scene.objects()) {
        GameObject& o = *up;
        if (o.isDestroyed()) continue;
        // Level objects physics moves.
        if (o.persistent) {
            if (auto* rb = o.getComponent<Rigidbody>(); rb && rb->type == BodyType::Dynamic) {
                const glm::vec3 p = o.transform().position();
                const glm::quat q = o.transform().rotation();
                moved[idToHex(o.id())] = {p.x, p.y, p.z, q.x, q.y, q.z, q.w};
            }
        }
        if (const auto* s = dynamic_cast<const Saveable*>(&o)) {
            const std::string key = objectKey(o);
            if (seen[key]++ == 1) log::warnOnce("Save: several objects named '" + o.name() + "' are Saveable; only one is saved");
            Json j = Json::object();
            s->saveState(j);
            state[key] = std::move(j);
        }
        for (const auto& c : o.components()) {
            if (const auto* s = dynamic_cast<const Saveable*>(c.get())) {
                Json j = Json::object();
                s->saveState(j);
                state[componentKey(o, *c)] = std::move(j);
            }
        }
    }
    for (uint64_t id : scene.levelObjects())
        if (!scene.findById(id)) removed.push_back(idToHex(id));
    // Prefab instances spawned while playing (player buildings): recreated on load.
    Json spawned = Json::array();
    const std::unordered_set<uint64_t> level(scene.levelObjects().begin(), scene.levelObjects().end());
    for (const auto& up : scene.objects()) {
        const GameObject& o = *up;
        const auto* pi = o.isDestroyed() ? nullptr : o.getComponent<PrefabInstance>();
        if (!pi || !pi->saveInGame || pi->prefab.empty() || level.count(o.id())) continue;
        if (o.transform().parent()) continue; // saved with its root
        const glm::vec3 p = o.transform().position(), sc = o.transform().localScale();
        const glm::quat q = o.transform().rotation();
        spawned.push_back({{"prefab", pi->prefab}, {"id", idToHex(o.id())}, {"position", {p.x, p.y, p.z}},
                           {"rotation", {q.x, q.y, q.z, q.w}}, {"scale", {sc.x, sc.y, sc.z}}});
    }
    return Json{{"version", 1}, {"moved", moved}, {"removed", removed}, {"spawned", spawned}, {"state", state}};
}

void apply(Scene& scene, const Json& save)
{
    if (save.value("version", 0) != 1) {
        log::warn("Save: unknown format");
        return;
    }
    for (const auto& id : save.value("removed", Json::array()))
        if (GameObject* o = scene.findById(idFromHex(id.get<std::string>()))) o->destroy();
    const Json moved = save.value("moved", Json::object());
    for (auto it = moved.begin(); it != moved.end(); ++it) {
        GameObject* o = scene.findById(idFromHex(it.key()));
        const Json& v = it.value();
        if (!o || !v.is_array() || v.size() != 7) continue;
        const glm::vec3 p{v[0].get<float>(), v[1].get<float>(), v[2].get<float>()};
        const glm::quat q{v[6].get<float>(), v[3].get<float>(), v[4].get<float>(), v[5].get<float>()};
        if (auto* rb = o->getComponent<Rigidbody>()) rb->teleport(p, q);
        else {
            o->transform().setPosition(p);
            o->transform().setRotation(q);
        }
    }
    // Runtime-spawned prefab instances: the save's set replaces whatever is there.
    const std::unordered_set<uint64_t> level(scene.levelObjects().begin(), scene.levelObjects().end());
    for (const auto& up : scene.objects()) {
        const auto* pi = up->isDestroyed() ? nullptr : up->getComponent<PrefabInstance>();
        if (pi && pi->saveInGame && !level.count(up->id()) && !up->transform().parent()) up->destroy();
    }
    std::vector<GameObject*> created;
    for (const Json& sp : save.value("spawned", Json::array())) {
        const auto vec = [&](const char* key, size_t n) {
            const Json& a = sp.value(key, Json::array());
            std::array<float, 4> v{0, 0, 0, 1};
            for (size_t i = 0; i < n && i < a.size(); ++i) v[i] = a[i].get<float>();
            return v;
        };
        const auto p = vec("position", 3), q = vec("rotation", 4), sc = vec("scale", 3);
        GameObject* o = prefab::instantiate(scene, sp.value("prefab", std::string()), {p[0], p[1], p[2]}, glm::quat(q[3], q[0], q[1], q[2]),
                                            nullptr, idFromHex(sp.value("id", std::string("0"))));
        if (!o) continue;
        if (sp.contains("scale")) o->transform().setLocalScale({sc[0], sc[1], sc[2]});
        created.push_back(o);
    }

    const Json& state = save.contains("state") ? save["state"] : Json::object();
    auto restore = [&](GameObject& o) {
        if (o.isDestroyed()) return;
        if (auto* s = dynamic_cast<Saveable*>(&o))
            if (auto it = state.find(objectKey(o)); it != state.end()) s->loadState(*it);
        for (const auto& c : o.components())
            if (auto* s = dynamic_cast<Saveable*>(c.get()))
                if (auto it = state.find(componentKey(o, *c)); it != state.end()) s->loadState(*it);
    };
    for (const auto& up : scene.objects()) restore(*up);
    // The new instances aren't in objects() until next frame.
    std::function<void(GameObject&)> subtree = [&](GameObject& o) {
        restore(o);
        for (Transform* c : o.transform().children()) subtree(c->gameObject());
    };
    for (GameObject* o : created) subtree(*o);
}

namespace {

// Temporary file + rename: a crash mid-write keeps the old file.
bool writeJsonAtomic(const std::filesystem::path& file, const Json& json)
{
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const std::filesystem::path tmp = file.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        if (!out) {
            log::error("Save: can't write {}", tmp.string());
            return false;
        }
        out << json.dump(1) << '\n';
        if (!out) return false;
    }
    std::filesystem::rename(tmp, file, ec);
    if (ec) {
        log::error("Save: {}", ec.message());
        return false;
    }
    return true;
}

std::optional<Json> readJson(const std::filesystem::path& file, const std::string& what)
{
    std::ifstream in(file);
    if (!in) return std::nullopt;
    try {
        Json j;
        in >> j;
        return j;
    } catch (const std::exception& e) {
        log::error("{} is damaged: {}", what, e.what());
        return std::nullopt;
    }
}

std::string safeName(const std::string& name, const char* fallback)
{
    std::string safe;
    for (char ch : name) safe += (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_') ? ch : '_';
    return safe.empty() ? std::string(fallback) : safe;
}

} // namespace

bool write(const std::string& slot, const Json& save)
{
    const std::filesystem::path file = slotPath(slot);
    if (!writeJsonAtomic(file, save)) return false;
    log::info("Saved '{}' ({})", slot, file.string());
    return true;
}

std::optional<Json> read(const std::string& slot)
{
    return readJson(slotPath(slot), "Save '" + slot + "'");
}

std::optional<Json> readConfig(const std::string& name)
{
    const std::string file = safeName(name, "config") + ".json";
    if (auto j = readJson(folder() / "config" / file, file)) return j;
    return readJson(folder() / file, file); // written by older builds, next to the saves
}

bool writeConfig(const std::string& name, const Json& json)
{
    const std::string file = safeName(name, "config") + ".json";
    if (!writeJsonAtomic(folder() / "config" / file, json)) return false;
    std::error_code ec;
    std::filesystem::remove(folder() / file, ec); // an older build's copy would show up as a save slot
    return true;
}

std::vector<SlotInfo> slots()
{
    std::vector<SlotInfo> out;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(folder(), ec)) {
        if (!e.is_regular_file() || e.path().extension() != ".json") continue;
        out.push_back({e.path().stem().string(), e.last_write_time(), e.file_size()});
    }
    std::sort(out.begin(), out.end(), [](const SlotInfo& a, const SlotInfo& b) { return a.time > b.time; });
    return out;
}

bool remove(const std::string& slot)
{
    std::error_code ec;
    return std::filesystem::remove(slotPath(slot), ec);
}

bool saveGame(Scene& scene, const std::string& slot, const Json& gameData)
{
    Json s = capture(scene);
    s["game"] = gameData;
    return write(slot, s);
}

std::optional<Json> loadGame(Scene& scene, const std::string& slot)
{
    auto s = read(slot);
    if (!s) return std::nullopt;
    save::apply(scene, *s);
    log::info("Loaded '{}'", slot);
    return s->value("game", Json::object());
}

} // namespace ve::save
