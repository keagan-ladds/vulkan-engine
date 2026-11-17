#include "ve/scene/SaveGame.hpp"

#include "ve/core/Engine.hpp"
#include "ve/core/Log.hpp"
#include "ve/physics/Physics.hpp"
#include "ve/scene/GameObject.hpp"
#include "ve/scene/Scene.hpp"

#include <SDL.h>

#include <algorithm>
#include <cctype>
#include <typeindex>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <unordered_map>

namespace ve::save {

namespace {

std::string hexId(uint64_t id)
{
    char b[17];
    std::snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(id));
    return b;
}

uint64_t parseId(const std::string& s)
{
    return std::strtoull(s.c_str(), nullptr, 16);
}

// Level objects are found by persistent id, code-spawned ones by name.
std::string objectKey(const GameObject& o)
{
    return o.persistent ? hexId(o.id()) : "name:" + o.name();
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
                moved[hexId(o.id())] = {p.x, p.y, p.z, q.x, q.y, q.z, q.w};
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
        if (!scene.findById(id)) removed.push_back(hexId(id));
    return Json{{"version", 1}, {"moved", moved}, {"removed", removed}, {"state", state}};
}

void apply(Scene& scene, const Json& save)
{
    if (save.value("version", 0) != 1) {
        log::warn("Save: unknown format");
        return;
    }
    for (const auto& id : save.value("removed", Json::array()))
        if (GameObject* o = scene.findById(parseId(id.get<std::string>()))) o->destroy();
    const Json moved = save.value("moved", Json::object());
    for (auto it = moved.begin(); it != moved.end(); ++it) {
        GameObject* o = scene.findById(parseId(it.key()));
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
    const Json& state = save.contains("state") ? save["state"] : Json::object();
    for (const auto& up : scene.objects()) {
        GameObject& o = *up;
        if (o.isDestroyed()) continue;
        if (auto* s = dynamic_cast<Saveable*>(&o))
            if (auto it = state.find(objectKey(o)); it != state.end()) s->loadState(*it);
        for (const auto& c : o.components())
            if (auto* s = dynamic_cast<Saveable*>(c.get()))
                if (auto it = state.find(componentKey(o, *c)); it != state.end()) s->loadState(*it);
    }
}

bool write(const std::string& slot, const Json& save)
{
    const std::filesystem::path file = slotPath(slot);
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    // Write a temporary file, then rename: a crash mid-save keeps the old save.
    const std::filesystem::path tmp = file.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        if (!out) {
            log::error("Save: can't write {}", tmp.string());
            return false;
        }
        out << save.dump(1) << '\n';
        if (!out) return false;
    }
    std::filesystem::rename(tmp, file, ec);
    if (ec) {
        log::error("Save: {}", ec.message());
        return false;
    }
    log::info("Saved '{}' ({})", slot, file.string());
    return true;
}

std::optional<Json> read(const std::string& slot)
{
    std::ifstream in(slotPath(slot));
    if (!in) return std::nullopt;
    try {
        Json j;
        in >> j;
        return j;
    } catch (const std::exception& e) {
        log::error("Save '{}' is damaged: {}", slot, e.what());
        return std::nullopt;
    }
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
