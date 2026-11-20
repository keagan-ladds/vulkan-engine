#include "ve/gameplay/ProxySet.hpp"

#include "ve/core/Log.hpp"
#include "ve/render/Renderer.hpp"
#include "ve/scene/Components.hpp"
#include "ve/scene/Ids.hpp"
#include "ve/scene/Prefab.hpp"
#include "ve/scene/Scene.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace ve {

namespace {

// Instance group sizes: the first holds this many, each further one twice the last.
constexpr uint32_t kFirstGroupCapacity = 256;
constexpr uint32_t kMaxGroupCapacity = 65536;

// Saveable state of an object and its components, keyed like save games do
// ("object", or the component's registered type name). Only registered
// component types can be found (and added back) when it's restored.
Json captureState(const GameObject& o)
{
    Json out = Json::object();
    if (const auto* s = dynamic_cast<const Saveable*>(&o)) {
        Json j = Json::object();
        s->saveState(j);
        out["object"] = std::move(j);
    }
    for (const auto& c : o.components()) {
        const auto* s = dynamic_cast<const Saveable*>(c.get());
        const TypeInfo* t = s ? TypeRegistry::get().of(*c) : nullptr;
        if (!t) continue;
        Json j = Json::object();
        s->saveState(j);
        out["components"][t->name] = std::move(j);
    }
    return out.empty() ? Json() : out;
}

void restoreState(GameObject& o, const Json& state)
{
    if (!state.is_object()) return;
    if (auto it = state.find("object"); it != state.end())
        if (auto* s = dynamic_cast<Saveable*>(&o)) s->loadState(*it);
    const Json& comps = state.contains("components") ? state["components"] : Json::object();
    for (auto it = comps.begin(); it != comps.end(); ++it) {
        Component* found = nullptr;
        for (const auto& c : o.components()) {
            const TypeInfo* t = TypeRegistry::get().of(*c);
            if (t && t->name == it.key()) found = c.get();
        }
        // A component the game added to the promoted object at runtime: add it again.
        if (!found)
            if (const TypeInfo* t = TypeRegistry::get().find(it.key()); t && t->isComponent && t->addTo) found = t->addTo(o);
        if (auto* s = dynamic_cast<Saveable*>(found)) s->loadState(it.value());
    }
}

bool sameEntry(const ProxyEntry& a, const ProxyEntry& b)
{
    constexpr float eps = 1e-4f;
    return a.type == b.type && a.state == b.state && glm::all(glm::lessThan(glm::abs(a.position - b.position), glm::vec3(eps))) &&
           std::abs(glm::dot(a.rotation, b.rotation)) > 1.0f - 1e-6f && glm::all(glm::lessThan(glm::abs(a.scale - b.scale), glm::vec3(eps))) &&
           a.data == b.data && a.objectState == b.objectState;
}

} // namespace

glm::mat4 ProxyEntry::matrix() const
{
    return glm::translate(glm::mat4(1.0f), position) * glm::mat4_cast(rotation) * glm::scale(glm::mat4(1.0f), scale);
}

// -----------------------------------------------------------------------------
// ProxyLink
// -----------------------------------------------------------------------------

ProxySet* ProxyLink::set() const
{
    return m_set.get(gameObject().scene());
}

const ProxyEntry* ProxyLink::entryData() const
{
    ProxySet* s = set();
    return s ? s->find(m_entry) : nullptr;
}

uint32_t ProxyLink::state() const
{
    const ProxyEntry* e = entryData();
    return e ? e->state : 0;
}

void ProxyLink::setState(uint32_t state)
{
    if (ProxySet* s = set()) s->setState(m_entry, state);
}

void ProxyLink::setData(Json data)
{
    if (ProxySet* s = set()) s->setData(m_entry, std::move(data));
}

void ProxyLink::consume()
{
    if (ProxySet* s = set()) {
        ++s->m_stats.consumed;
        s->remove(m_entry); // destroys this object too
    } else {
        gameObject().destroy();
    }
}

// -----------------------------------------------------------------------------
// ProxySet: types and entries
// -----------------------------------------------------------------------------

ProxySet::ProxySet(std::string name) : GameObject(std::move(name)) {}

ProxySet::~ProxySet() = default;

uint32_t ProxySet::addType(ProxyType type)
{
    const uint32_t index = typeIndexOrAdd(type.name);
    TypeData& t = m_types[index];
    // Redefined: its entries are drawn again with the new look.
    for (auto& [id, s] : m_entries)
        if (s.e.type == index) hide(s);
    t.groups.clear();
    t.desc = std::move(type);
    t.defined = true;
    for (auto& [id, s] : m_entries)
        if (s.e.type == index) show(s);
    m_rescan = true;
    return index;
}

uint32_t ProxySet::typeIndexOrAdd(const std::string& name)
{
    if (int i = typeIndex(name); i >= 0) return uint32_t(i);
    TypeData t;
    t.desc.name = name;
    m_types.push_back(std::move(t));
    return uint32_t(m_types.size() - 1);
}

int ProxySet::typeIndex(const std::string& name) const
{
    for (size_t i = 0; i < m_types.size(); ++i)
        if (m_types[i].desc.name == name) return int(i);
    return -1;
}

const ProxyType* ProxySet::type(uint32_t index) const
{
    return index < m_types.size() && m_types[index].defined ? &m_types[index].desc : nullptr;
}

ProxySet::Slot* ProxySet::slot(uint64_t id)
{
    auto it = m_entries.find(id);
    return it != m_entries.end() ? &it->second : nullptr;
}

const ProxySet::Slot* ProxySet::slot(uint64_t id) const
{
    auto it = m_entries.find(id);
    return it != m_entries.end() ? &it->second : nullptr;
}

uint64_t ProxySet::add(uint32_t type, const glm::vec3& position, const glm::quat& rotation, const glm::vec3& scale, uint32_t state,
                       Json data, uint64_t id)
{
    if (type >= m_types.size()) {
        log::warn("ProxySet '{}': add() with unknown type {}", name(), type);
        return 0;
    }
    // Level content (before start) counts up, so the same build gives the same
    // ids; runtime spawns get random ones (they never collide with a level's).
    if (id == 0) id = m_hasBaseline ? newObjectId() : m_nextId++;
    else if (!m_hasBaseline) m_nextId = std::max(m_nextId, id + 1);
    if (m_entries.count(id)) {
        log::warn("ProxySet '{}': entry {} exists already", name(), idToHex(id));
        return 0;
    }
    ProxyEntry e;
    e.id = id;
    e.type = type;
    e.position = position;
    e.rotation = rotation;
    e.scale = scale;
    e.state = state;
    e.data = std::move(data);
    upsert(e);
    return id;
}

void ProxySet::upsert(const ProxyEntry& e)
{
    if (Slot* s = slot(e.id)) {
        place(*s, e);
        return;
    }
    Slot& s = m_entries[e.id];
    s.e = e;
    gridAdd(s.e);
    show(s);
    m_rescan = true;
}

void ProxySet::place(Slot& s, const ProxyEntry& from)
{
    const bool moved = from.position != s.e.position || from.rotation != s.e.rotation || from.scale != s.e.scale;
    const bool retyped = from.type != s.e.type;
    if (s.promoted && (moved || retyped)) { // the object stands for the old one: let it come back as the new
        if (GameObject* obj = scene().findById(s.promoted)) obj->destroy();
        s.promoted = 0;
        std::erase(m_promotedIds, s.e.id);
    }
    if (retyped) hide(s);
    gridRemove(s.e);
    s.e = from;
    gridAdd(s.e);
    if (s.group >= 0 && moved) m_types[s.e.type].groups[size_t(s.group)]->set(s.handle, s.e.matrix());
    show(s);
    m_rescan = true;
}

bool ProxySet::remove(uint64_t id)
{
    if (!slot(id)) return false;
    erase(id, true);
    return true;
}

void ProxySet::erase(uint64_t id, bool destroyObject)
{
    Slot* s = slot(id);
    if (!s) return;
    if (s->promoted) {
        if (GameObject* obj = destroyObject ? scene().findById(s->promoted) : nullptr) obj->destroy();
        std::erase(m_promotedIds, id);
    }
    hide(*s);
    gridRemove(s->e);
    m_entries.erase(id);
}

const ProxyEntry* ProxySet::find(uint64_t id) const
{
    const Slot* s = slot(id);
    return s ? &s->e : nullptr;
}

bool ProxySet::setState(uint64_t id, uint32_t state)
{
    Slot* s = slot(id);
    if (!s) return false;
    s->e.state = state;
    return true;
}

bool ProxySet::setData(uint64_t id, Json data)
{
    Slot* s = slot(id);
    if (!s) return false;
    s->e.data = std::move(data);
    return true;
}

bool ProxySet::setTransform(uint64_t id, const glm::vec3& position, const glm::quat& rotation, const glm::vec3& scale)
{
    Slot* s = slot(id);
    if (!s) return false;
    if (GameObject* obj = s->promoted ? scene().findById(s->promoted) : nullptr) { // the object is the entry now: move it
        obj->transform().setPosition(position);
        obj->transform().setRotation(rotation);
        obj->transform().setLocalScale(scale);
    }
    ProxyEntry e = s->e;
    e.position = position;
    e.rotation = rotation;
    e.scale = scale;
    if (s->promoted) { // (keep the object: only the record follows it)
        gridRemove(s->e);
        s->e = e;
        gridAdd(s->e);
    } else {
        place(*s, e);
    }
    return true;
}

void ProxySet::forEach(const std::function<void(const ProxyEntry&)>& fn) const
{
    for (const auto& [id, s] : m_entries) fn(s.e);
}

int64_t ProxySet::cellKey(int cx, int cz)
{
    return (int64_t(cx) << 32) ^ int64_t(uint32_t(cz));
}

int ProxySet::cellOf(float v) const
{
    return int(std::floor(v / m_cellSize));
}

void ProxySet::gridAdd(const ProxyEntry& e)
{
    m_grid[cellKey(cellOf(e.position.x), cellOf(e.position.z))].push_back(e.id);
}

void ProxySet::gridRemove(const ProxyEntry& e)
{
    auto it = m_grid.find(cellKey(cellOf(e.position.x), cellOf(e.position.z)));
    if (it == m_grid.end()) return;
    std::erase(it->second, e.id);
    if (it->second.empty()) m_grid.erase(it);
}

void ProxySet::query(const glm::vec3& p, float radius, const std::function<void(const ProxyEntry&)>& fn) const
{
    const int x0 = cellOf(p.x - radius), x1 = cellOf(p.x + radius);
    const int z0 = cellOf(p.z - radius), z1 = cellOf(p.z + radius);
    for (int cz = z0; cz <= z1; ++cz) {
        for (int cx = x0; cx <= x1; ++cx) {
            auto it = m_grid.find(cellKey(cx, cz));
            if (it == m_grid.end()) continue;
            for (uint64_t id : it->second) {
                const Slot* s = slot(id);
                if (s && glm::distance(s->e.position, p) <= radius) fn(s->e);
            }
        }
    }
}

const ProxyEntry* ProxySet::nearest(const glm::vec3& p, float radius, int type) const
{
    const ProxyEntry* best = nullptr;
    float bestDist = radius;
    query(p, radius, [&](const ProxyEntry& e) {
        const float d = glm::distance(e.position, p);
        if ((type < 0 || e.type == uint32_t(type)) && d <= bestDist) {
            best = &e;
            bestDist = d;
        }
    });
    return best;
}

// -----------------------------------------------------------------------------
// Drawing: one InstanceGroup chain per type
// -----------------------------------------------------------------------------

void ProxySet::show(Slot& s)
{
    if (s.group >= 0 || s.promoted || s.e.type >= m_types.size()) return;
    TypeData& t = m_types[s.e.type];
    if (!t.defined || t.desc.lods.empty()) return;
    size_t g = 0;
    while (g < t.groups.size() && t.groups[g]->count() >= t.groups[g]->capacity()) ++g;
    if (g == t.groups.size()) { // all full (or none yet): a bigger one
        InstanceGroupDesc desc;
        desc.name = name() + ": " + t.desc.name;
        desc.lods = t.desc.lods;
        desc.castShadows = t.desc.castShadows;
        desc.cullDistance = t.desc.cullDistance;
        desc.shadowDistance = t.desc.shadowDistance;
        const uint32_t capacity = t.groups.empty() ? kFirstGroupCapacity : std::min(kMaxGroupCapacity, t.groups.back()->capacity() * 2);
        t.groups.push_back(renderer().createInstanceGroup(std::move(desc), capacity));
    }
    s.group = int(g);
    s.handle = t.groups[g]->add(s.e.matrix());
}

void ProxySet::hide(Slot& s)
{
    if (s.group < 0) return;
    TypeData& t = m_types[s.e.type];
    if (size_t(s.group) < t.groups.size()) t.groups[size_t(s.group)]->remove(s.handle);
    s.group = -1;
    s.handle = InstanceGroup::kInvalid;
}

// -----------------------------------------------------------------------------
// Promotion
// -----------------------------------------------------------------------------

bool ProxySet::focusPosition(glm::vec3* out) const
{
    if (const GameObject* f = focus.get(scene())) {
        *out = f->transform().position();
        return true;
    }
    if (const Camera* cam = scene().mainCamera()) {
        *out = cam->transform().position();
        return true;
    }
    return false;
}

const ProxyStats& ProxySet::stats() const
{
    m_stats.entries = uint32_t(m_entries.size());
    m_stats.promoted = uint32_t(m_promotedIds.size());
    return m_stats;
}

GameObject* ProxySet::promoted(uint64_t id) const
{
    const Slot* s = slot(id);
    return s && s->promoted ? scene().findById(s->promoted) : nullptr;
}

void ProxySet::promote(Slot& s)
{
    const TypeData& t = m_types[s.e.type];
    GameObject* obj = nullptr;
    if (t.desc.promote) obj = t.desc.promote(scene(), s.e);
    else if (!t.desc.prefab.empty()) obj = prefab::instantiate(scene(), t.desc.prefab, s.e.position, s.e.rotation);
    else obj = &scene().spawn(t.desc.name);
    if (!obj) return; // (the factory says "not now": asked again when the focus moves)
    // The set saves it, as its entry: save games mustn't recreate the object too.
    if (auto* pi = obj->getComponent<PrefabInstance>()) pi->saveInGame = false;
    obj->transform().setPosition(s.e.position);
    obj->transform().setRotation(s.e.rotation);
    obj->transform().setLocalScale(s.e.scale);
    // Unless the object brings its own, it looks and collides like the instance (an invisible swap).
    if (!obj->getComponent<MeshRenderer>() && !t.desc.lods.empty())
        obj->addComponent<MeshRenderer>(t.desc.lods.front().mesh, t.desc.lods.front().materials);
    if (t.desc.collider.valid() && !obj->getComponent<Rigidbody>()) obj->addComponent<Rigidbody>(t.desc.collider, BodyType::Static);
    auto& link = obj->addComponent<ProxyLink>();
    link.m_set = this;
    link.m_entry = s.e.id;
    restoreState(*obj, s.e.objectState);
    hide(s);
    s.promoted = obj->id();
    m_promotedIds.push_back(s.e.id);
    ++m_stats.promotions;
}

void ProxySet::demote(Slot& s)
{
    if (!s.promoted) return;
    if (GameObject* obj = scene().findById(s.promoted)) {
        // What the object became (moved, its Saveable state) lives on in the entry.
        if (auto e = current(s)) {
            gridRemove(s.e);
            s.e = std::move(*e);
            gridAdd(s.e);
        }
        obj->destroy();
    }
    s.promoted = 0;
    std::erase(m_promotedIds, s.e.id);
    show(s);
    ++m_stats.demotions;
}

void ProxySet::start()
{
    // Everything added so far is the level's content: save games store the difference to it.
    if (!m_hasBaseline) {
        m_baseline.clear();
        for (const auto& [id, s] : m_entries) m_baseline[id] = s.e;
        m_hasBaseline = true;
    }
}

void ProxySet::update(float)
{
    glm::vec3 f;
    if (!focusPosition(&f)) return; // (no camera yet: nothing to measure from)
    // Hand back what's out of reach; forget what gameplay destroyed (picked up).
    uint32_t demotions = 0;
    for (size_t k = 0; k < m_promotedIds.size();) {
        const uint64_t id = m_promotedIds[k];
        Slot* s = slot(id);
        GameObject* obj = s ? scene().findById(s->promoted) : nullptr;
        if (!s || !obj) {
            if (s) ++m_stats.consumed;
            erase(id, false); // (erases m_promotedIds[k])
            if (!s) m_promotedIds.erase(m_promotedIds.begin() + ptrdiff_t(k));
            continue;
        }
        const ProxyType& t = m_types[s->e.type].desc;
        const float far = t.promoteDistance * (1.0f + t.demoteMargin);
        if (glm::distance(obj->transform().position(), f) > far && demotions < demotionsPerFrame) {
            demote(*s); // (erases m_promotedIds[k])
            ++demotions;
            continue;
        }
        ++k;
    }

    // Promote what came close (only when the focus moved, or something changed).
    if (!m_rescan && glm::distance(f, m_scanFocus) < 0.5f) return;
    m_rescan = false;
    m_scanFocus = f;
    float reach = 0.0f;
    for (const TypeData& t : m_types)
        if (t.defined) reach = std::max(reach, t.desc.promoteDistance);
    if (reach <= 0.0f) return;
    std::vector<uint64_t> wanted;
    query(f, reach, [&](const ProxyEntry& e) {
        const TypeData& t = m_types[e.type];
        if (t.defined && t.desc.promoteDistance > 0.0f && !slot(e.id)->promoted && glm::distance(e.position, f) < t.desc.promoteDistance)
            wanted.push_back(e.id);
    });
    // Nearest first, so a budget-limited frame does what matters.
    std::sort(wanted.begin(), wanted.end(), [&](uint64_t a, uint64_t b) {
        return glm::distance(slot(a)->e.position, f) < glm::distance(slot(b)->e.position, f);
    });
    for (size_t i = 0; i < wanted.size(); ++i) {
        if (i >= promotionsPerFrame) {
            m_rescan = true; // the rest next frame
            break;
        }
        promote(*slot(wanted[i]));
    }
}

void ProxySet::onDestroy()
{
    // The objects are the set's: they go with it (their state with them).
    for (uint64_t id : m_promotedIds) {
        Slot* s = slot(id);
        if (GameObject* obj = s ? scene().findById(s->promoted) : nullptr) obj->destroy();
        if (s) s->promoted = 0;
    }
    m_promotedIds.clear();
    for (auto& [id, s] : m_entries) {
        s.group = -1;
        s.handle = InstanceGroup::kInvalid;
    }
    for (TypeData& t : m_types) t.groups.clear();
}

// -----------------------------------------------------------------------------
// Persistence
// -----------------------------------------------------------------------------

std::optional<ProxyEntry> ProxySet::current(const Slot& s) const
{
    if (!s.promoted) return s.e;
    const GameObject* obj = scene().findById(s.promoted);
    if (!obj) return std::nullopt; // destroyed by gameplay: consumed
    ProxyEntry e = s.e;
    e.position = obj->transform().position();
    e.rotation = obj->transform().rotation();
    e.scale = obj->transform().localScale();
    e.objectState = captureState(*obj);
    return e;
}

// [id, type, px, py, pz, qx, qy, qz, qw, sx, sy, sz, state, {data, object}?]: compact, as there can be thousands.
Json ProxySet::entryJson(const ProxyEntry& e, std::unordered_map<uint32_t, uint32_t>& typeMap, Json& types) const
{
    auto [it, fresh] = typeMap.try_emplace(e.type, uint32_t(types.size()));
    if (fresh) types.push_back(e.type < m_types.size() ? m_types[e.type].desc.name : std::string("?"));
    Json j = {idToHex(e.id), it->second, e.position.x, e.position.y, e.position.z, e.rotation.x, e.rotation.y, e.rotation.z,
              e.rotation.w, e.scale.x, e.scale.y, e.scale.z, e.state};
    if (!e.data.is_null() || !e.objectState.is_null()) {
        Json extra = Json::object();
        if (!e.data.is_null()) extra["data"] = e.data;
        if (!e.objectState.is_null()) extra["object"] = e.objectState;
        j.push_back(std::move(extra));
    }
    return j;
}

std::optional<ProxyEntry> ProxySet::entryFromJson(const Json& j, const std::vector<uint32_t>& typeMap) const
{
    if (!j.is_array() || j.size() < 13 || !j[0].is_string()) return std::nullopt;
    for (size_t i = 1; i < 13; ++i)
        if (!j[i].is_number()) return std::nullopt;
    ProxyEntry e;
    e.id = idFromHex(j[0].get<std::string>());
    const uint32_t t = j[1].get<uint32_t>();
    if (e.id == 0 || t >= typeMap.size()) return std::nullopt;
    e.type = typeMap[t];
    e.position = {j[2].get<float>(), j[3].get<float>(), j[4].get<float>()};
    e.rotation = glm::quat(j[8].get<float>(), j[5].get<float>(), j[6].get<float>(), j[7].get<float>());
    e.scale = {j[9].get<float>(), j[10].get<float>(), j[11].get<float>()};
    e.state = j[12].get<uint32_t>();
    if (j.size() > 13 && j[13].is_object()) {
        e.data = j[13].value("data", Json());
        e.objectState = j[13].value("object", Json());
    }
    return e;
}

Json ProxySet::entriesJson() const
{
    // Sorted by id: the scene file doesn't churn between saves.
    std::vector<uint64_t> ids;
    ids.reserve(m_entries.size());
    for (const auto& [id, s] : m_entries) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    std::unordered_map<uint32_t, uint32_t> typeMap;
    Json types = Json::array(), entries = Json::array();
    for (uint64_t id : ids)
        if (auto e = current(*slot(id))) entries.push_back(entryJson(*e, typeMap, types));
    return {{"types", types}, {"entries", entries}};
}

void ProxySet::setEntriesJson(const Json& json)
{
    if (!json.is_object()) return;
    std::vector<uint32_t> typeMap;
    for (const Json& n : json.value("types", Json::array())) typeMap.push_back(typeIndexOrAdd(n.is_string() ? n.get<std::string>() : "?"));
    std::vector<uint64_t> old;
    for (const auto& [id, s] : m_entries) old.push_back(id);
    for (uint64_t id : old) erase(id, true);
    for (const Json& j : json.value("entries", Json::array())) {
        if (auto e = entryFromJson(j, typeMap)) {
            upsert(*e);
            m_nextId = std::max(m_nextId, e->id + 1);
        }
    }
    // Authored content (the scene file, an editor edit): it's the level's from now on.
    if (m_hasBaseline) {
        m_baseline.clear();
        for (const auto& [id, s] : m_entries) m_baseline[id] = s.e;
    }
}

void ProxySet::saveState(Json& out) const
{
    // The difference to the level's entries (taken at start: nothing to compare against before).
    std::unordered_map<uint32_t, uint32_t> typeMap;
    Json types = Json::array(), entries = Json::array(), removed = Json::array();
    std::vector<uint64_t> ids;
    for (const auto& [id, s] : m_entries) ids.push_back(id);
    for (const auto& [id, e] : m_baseline)
        if (!m_entries.count(id)) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    for (uint64_t id : ids) {
        const Slot* s = slot(id);
        const std::optional<ProxyEntry> now = s ? current(*s) : std::nullopt;
        const auto base = m_baseline.find(id);
        if (!now) {
            if (base != m_baseline.end()) removed.push_back(idToHex(id));
        } else if (base == m_baseline.end() || !sameEntry(*now, base->second)) {
            entries.push_back(entryJson(*now, typeMap, types));
        }
    }
    out["types"] = std::move(types);
    out["entries"] = std::move(entries);
    out["removed"] = std::move(removed);
}

void ProxySet::loadState(const Json& in)
{
    if (!m_hasBaseline) start();
    // The target: the level's entries, minus the removed ones, with the saved ones over them.
    std::unordered_map<uint64_t, ProxyEntry> target = m_baseline;
    for (const Json& id : in.value("removed", Json::array()))
        if (id.is_string()) target.erase(idFromHex(id.get<std::string>()));
    std::vector<uint32_t> typeMap;
    for (const Json& n : in.value("types", Json::array())) typeMap.push_back(typeIndexOrAdd(n.is_string() ? n.get<std::string>() : "?"));
    for (const Json& j : in.value("entries", Json::array()))
        if (auto e = entryFromJson(j, typeMap)) target[e->id] = std::move(*e);

    // Promoted objects stand for what was there before: they go (promoted again next frame).
    for (uint64_t id : m_promotedIds) {
        Slot* s = slot(id);
        if (GameObject* obj = s ? scene().findById(s->promoted) : nullptr) obj->destroy();
        if (s) {
            s->promoted = 0;
            show(*s);
        }
    }
    m_promotedIds.clear();
    // Reconcile: only what differs is touched (a level of thousands, a save of a few).
    std::vector<uint64_t> gone;
    for (const auto& [id, s] : m_entries)
        if (!target.count(id)) gone.push_back(id);
    for (uint64_t id : gone) erase(id, true);
    for (const auto& [id, e] : target) {
        const Slot* s = slot(id);
        if (!s || !sameEntry(s->e, e)) upsert(e);
    }
    m_rescan = true;
}

// -----------------------------------------------------------------------------
// Reflection
// -----------------------------------------------------------------------------

void registerProxyTypes(TypeRegistry& r)
{
    r.add<ProxySet>("ProxySet", [](TypeBuilder<ProxySet>& t) {
        t.category("Gameplay");
        t.property(
             "entryCount", FieldKind::Int, [](const ProxySet& s) { return Json(s.size()); }, [](ProxySet&, const Json&) {})
            .readOnly()
            .tooltip("things held as data (drawn instanced, objects only near the player); the game adds their types in code");
        t.property("promotionsPerFrame", FieldKind::Int, [](const ProxySet& s) { return Json(s.promotionsPerFrame); },
                   [](ProxySet& s, const Json& j) { if (j.is_number()) s.promotionsPerFrame = uint32_t(std::max(1, j.get<int>())); });
        t.property("entries", FieldKind::Json, [](const ProxySet& s) { return s.entriesJson(); },
                   [](ProxySet& s, const Json& j) { s.setEntriesJson(j); })
            .hidden();
    });
}

} // namespace ve
