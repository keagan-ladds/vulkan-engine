#include "ve/scene/SceneSerializer.hpp"

#include "ve/assets/AssetManager.hpp"
#include "ve/core/Engine.hpp"
#include "ve/core/Log.hpp"

#include <glm/gtc/quaternion.hpp>

#include <cstdio>
#include <fstream>
#include <functional>
#include <random>

namespace ve {

namespace {
uint64_t randomId()
{
    static std::mt19937_64 rng{std::random_device{}()};
    uint64_t id = 0;
    while (id == 0) id = rng();
    return id;
}

bool isPersistentRoot(const GameObject& o)
{
    if (!o.persistent || o.isDestroyed()) return false;
    const Transform* p = o.transform().parent();
    return !p || !p->gameObject().persistent;
}
} // namespace

std::string SceneSerializer::idToString(uint64_t id)
{
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(id));
    return buf;
}

uint64_t SceneSerializer::idFromString(const std::string& s)
{
    try {
        return std::stoull(s, nullptr, 16);
    } catch (...) {
        return 0;
    }
}

Json SceneSerializer::serializeFields(const TypeInfo& type, const void* object)
{
    Json fields = Json::object();
    for (const FieldInfo& f : type.fields) fields[f.name] = f.get(object);
    return fields;
}

void SceneSerializer::applyFields(const TypeInfo& type, void* object, const Json& fields)
{
    if (!fields.is_object()) return;
    for (const FieldInfo& f : type.fields) {
        auto it = fields.find(f.name);
        if (it != fields.end()) f.set(object, *it);
    }
}

Json SceneSerializer::serializeComponent(const Component& c)
{
    const TypeInfo* type = TypeRegistry::get().of(c);
    Json j;
    j["type"] = type ? type->name : std::string("?");
    j["enabled"] = c.enabled;
    if (type) j["fields"] = serializeFields(*type, &c);
    return j;
}

Json SceneSerializer::serializeObject(const GameObject& o, bool worldTransform)
{
    const TypeInfo* type = TypeRegistry::get().of(o);
    Json j;
    j["id"] = idToString(o.id());
    j["type"] = type ? type->name : std::string("GameObject");
    j["name"] = o.name();
    j["active"] = o.activeSelf();
    if (!o.tag.empty()) j["tag"] = o.tag;
    const Transform& t = o.transform();
    const glm::vec3 p = worldTransform ? t.position() : t.localPosition();
    const glm::quat q = worldTransform ? t.rotation() : t.localRotation();
    glm::vec3 s = t.localScale();
    if (worldTransform) {
        const glm::mat4& m = t.worldMatrix();
        s = {glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2]))};
    }
    j["position"] = {p.x, p.y, p.z};
    j["rotation"] = {q.x, q.y, q.z, q.w};
    j["scale"] = {s.x, s.y, s.z};
    if (type && !type->fields.empty()) j["fields"] = serializeFields(*type, &o);

    Json comps = Json::array();
    for (const auto& c : o.components()) {
        if (c->persistent && TypeRegistry::get().of(*c)) comps.push_back(serializeComponent(*c));
    }
    if (!comps.empty()) j["components"] = std::move(comps);

    Json children = Json::array();
    for (const Transform* child : t.children()) {
        const GameObject& co = child->gameObject();
        if (co.persistent && !co.isDestroyed()) children.push_back(serializeObject(co));
    }
    if (!children.empty()) j["children"] = std::move(children);
    return j;
}

Json SceneSerializer::serializeScene(const Scene& scene)
{
    Json objects = Json::array();
    for (const auto& o : scene.objects()) {
        if (isPersistentRoot(*o)) objects.push_back(serializeObject(*o, o->transform().parent() != nullptr));
    }
    return Json{{"version", 1}, {"objects", std::move(objects)}};
}

namespace {
void applyTransformAndBasics(GameObject& o, const Json& j)
{
    if (auto it = j.find("name"); it != j.end() && it->is_string()) o.setName(it->get<std::string>());
    o.setActive(j.value("active", true));
    o.tag = j.value("tag", std::string());
    Transform& t = o.transform();
    t.setLocalPosition(detail::vecFromJson(j.value("position", Json()), glm::vec3(0.0f)));
    const glm::vec4 q = detail::vecFromJson(j.value("rotation", Json()), glm::vec4(0, 0, 0, 1));
    t.setLocalRotation(glm::normalize(glm::quat(q.w, q.x, q.y, q.z)));
    t.setLocalScale(detail::vecFromJson(j.value("scale", Json()), glm::vec3(1.0f)));
}
} // namespace

GameObject* SceneSerializer::createObject(Scene& scene, const Json& j, GameObject* parent, bool newIds)
{
    const std::string typeName = j.value("type", std::string("GameObject"));
    const TypeInfo* type = TypeRegistry::get().find(typeName);
    if (!type || !type->spawn) {
        log::warn("Scene: unknown or non-creatable type '{}' ({}); using a plain GameObject", typeName,
                  j.value("name", std::string()));
        type = TypeRegistry::get().find("GameObject");
    }
    GameObject* o = type->spawn(scene);
    o->persistent = true;
    const uint64_t id = newIds ? 0 : idFromString(j.value("id", std::string()));
    o->setId(id != 0 && !scene.findById(id) ? id : randomId());
    if (parent) o->setParent(parent, false);
    applyTransformAndBasics(*o, j);
    if (auto it = j.find("fields"); it != j.end()) applyFields(*type, o, *it);

    if (auto it = j.find("components"); it != j.end() && it->is_array()) {
        for (const Json& cj : *it) {
            const std::string cname = cj.value("type", std::string());
            const TypeInfo* ct = TypeRegistry::get().find(cname);
            if (!ct || !ct->addTo) {
                log::warn("Scene: unknown component '{}' on '{}'", cname, o->name());
                continue;
            }
            Component* c = ct->addTo(*o);
            c->persistent = true;
            c->enabled = cj.value("enabled", true);
            if (auto f = cj.find("fields"); f != cj.end()) applyFields(*ct, c, *f);
        }
    }
    if (auto it = j.find("children"); it != j.end() && it->is_array()) {
        for (const Json& cj : *it) createObject(scene, cj, o, newIds);
    }
    return o;
}

bool SceneSerializer::applyObject(GameObject& o, const Json& j)
{
    // Same persistent component types in the same order? Otherwise re-create.
    std::vector<Component*> comps;
    for (const auto& c : o.components()) {
        if (c->persistent) comps.push_back(c.get());
    }
    const Json cjs = j.value("components", Json::array());
    if (cjs.size() != comps.size()) return false;
    for (size_t i = 0; i < comps.size(); ++i) {
        const TypeInfo* ct = TypeRegistry::get().of(*comps[i]);
        if (!ct || ct->name != cjs[i].value("type", std::string())) return false;
    }
    applyTransformAndBasics(o, j);
    const TypeInfo* type = TypeRegistry::get().of(o);
    if (type) {
        if (auto it = j.find("fields"); it != j.end()) {
            applyFields(*type, &o, *it);
            if (type->onChanged) type->onChanged(&o);
        }
    }
    for (size_t i = 0; i < comps.size(); ++i) {
        const TypeInfo* ct = TypeRegistry::get().of(*comps[i]);
        comps[i]->enabled = cjs[i].value("enabled", true);
        applyFields(*ct, comps[i], cjs[i].value("fields", Json::object()));
        if (ct->onChanged) ct->onChanged(comps[i]);
    }
    return true;
}

void SceneSerializer::clearPersistent(Scene& scene)
{
    std::vector<GameObject*> roots;
    for (const auto& o : scene.objects()) {
        if (isPersistentRoot(*o)) roots.push_back(o.get());
    }
    for (GameObject* o : roots) o->destroy();
}

std::filesystem::path SceneSerializer::writablePath(const std::filesystem::path& relative)
{
    if (relative.is_absolute()) return relative;
    const auto& dirs = Engine::get().assets().searchPaths();
    return dirs.empty() ? relative : dirs.front() / relative;
}

bool SceneSerializer::save(const Scene& scene, const std::filesystem::path& path)
{
    const std::filesystem::path file = writablePath(path);
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file);
    if (!out) {
        log::error("Scene: can't write {}", file.string());
        return false;
    }
    out << serializeScene(scene).dump(2) << '\n';
    s_lastPath = path;
    log::info("Scene saved: {}", file.string());
    return true;
}

std::vector<GameObject*> SceneSerializer::loadJson(Scene& scene, const Json& json)
{
    std::vector<GameObject*> roots;
    for (const Json& oj : json.value("objects", Json::array())) roots.push_back(createObject(scene, oj));
    // Remember the level's objects (save games record which ones are gone).
    std::function<void(GameObject&)> mark = [&](GameObject& o) {
        if (o.persistent) scene.addLevelObject(o.id());
        for (Transform* c : o.transform().children()) mark(c->gameObject());
    };
    for (GameObject* r : roots)
        if (r) mark(*r);
    return roots;
}

std::vector<GameObject*> SceneSerializer::load(Scene& scene, const std::filesystem::path& path)
{
    std::filesystem::path file;
    try {
        file = Engine::get().assets().resolve(path);
    } catch (const std::exception& e) {
        log::warn("Scene: {}", e.what());
        return {};
    }
    std::ifstream in(file);
    Json json;
    try {
        in >> json;
    } catch (const std::exception& e) {
        log::error("Scene: {} is not valid JSON: {}", file.string(), e.what());
        return {};
    }
    s_lastPath = path;
    auto roots = loadJson(scene, json);
    log::info("Scene loaded: {} ({} root objects)", path.string(), roots.size());
    return roots;
}

} // namespace ve
