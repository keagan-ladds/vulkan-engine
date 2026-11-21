#include "ve/render/StaticBatcher.hpp"

#include "ve/render/Material.hpp"
#include "ve/render/Renderer.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace ve {

namespace {
// New groups start small (most batches hold a handful of pieces) and double.
constexpr uint32_t kInitialCapacity = 16;
} // namespace

StaticBatcher::Key StaticBatcher::makeKey(const Mesh* mesh, const std::vector<std::shared_ptr<Material>>& materials,
                                          bool castShadows)
{
    std::vector<const Material*> mats;
    mats.reserve(materials.size());
    for (const auto& m : materials) mats.push_back(m.get());
    return {mesh, std::move(mats), castShadows};
}

InstanceGroupDesc StaticBatcher::groupDesc(const Group& g) const
{
    InstanceGroupDesc desc;
    desc.name = "static " + g.mesh->name();
    desc.lods = {{g.mesh, g.materials}};
    desc.castShadows = g.castShadows;
    return desc;
}

StaticBatcher::Group& StaticBatcher::groupFor(const Desc& desc)
{
    Key key = makeKey(desc.mesh.get(), desc.materials, desc.castShadows);
    auto it = m_groupByKey.find(key);
    if (it != m_groupByKey.end()) return *it->second;
    auto g = std::make_unique<Group>();
    g->key = key;
    g->mesh = desc.mesh;
    g->materials = desc.materials;
    g->castShadows = desc.castShadows;
    g->instances = m_renderer->createInstanceGroup(groupDesc(*g), kInitialCapacity);
    g->instances->setHidden(m_hidden);
    return *m_groupByKey.emplace(std::move(key), std::move(g)).first->second;
}

void StaticBatcher::grow(Group& g)
{
    // Full: a group twice the size takes the instances (handles change, the
    // entries follow), and the old one goes (its GPU range is freed once the
    // frames in flight are done with it).
    const uint32_t capacity = g.instances->capacity() * 2;
    std::shared_ptr<InstanceGroup> bigger = m_renderer->createInstanceGroup(groupDesc(g), capacity);
    bigger->setHidden(m_hidden);
    std::vector<Handle> entryOf;
    entryOf.reserve(capacity);
    for (InstanceGroup::Handle old = 0; old < g.entryOfInstance.size(); ++old) {
        const Handle e = g.entryOfInstance[old];
        if (e == kNone) continue;
        const InstanceGroup::Handle h = bigger->add(m_entries[e].world);
        if (entryOf.size() <= h) entryOf.resize(h + 1, kNone);
        entryOf[h] = e;
        m_entries[e].instance = h;
    }
    g.instances = std::move(bigger);
    g.entryOfInstance = std::move(entryOf);
}

void StaticBatcher::releaseGroup(Group& g)
{
    // (Destroying the InstanceGroup defers freeing its GPU range.)
    m_groupByKey.erase(g.key);
}

StaticBatcher::Handle StaticBatcher::add(const Desc& desc)
{
    Group& g = groupFor(desc);
    if (g.count >= g.instances->capacity()) grow(g);
    Handle h;
    if (!m_free.empty()) {
        h = m_free.back();
        m_free.pop_back();
    } else {
        h = static_cast<Handle>(m_entries.size());
        m_entries.emplace_back();
    }
    Entry& e = m_entries[h];
    e = Entry{};
    e.group = &g;
    e.world = desc.world;
    e.bounds = desc.mesh->bounds().transformed(desc.world);
    e.owner = desc.owner;
    e.source = desc.source;
    e.instance = g.instances->add(desc.world);
    if (g.entryOfInstance.size() <= e.instance) g.entryOfInstance.resize(e.instance + 1, kNone);
    g.entryOfInstance[e.instance] = h;
    ++g.count;
    m_submeshes += static_cast<uint32_t>(desc.mesh->submeshes().size());
    if (!m_topDirty) m_top = std::max(m_top, e.bounds.max.y);
    insertGrid(h);
    return h;
}

void StaticBatcher::remove(Handle h)
{
    if (h >= m_entries.size() || !m_entries[h].group) return;
    Entry& e = m_entries[h];
    Group& g = *e.group;
    eraseGrid(h);
    g.instances->remove(e.instance);
    g.entryOfInstance[e.instance] = kNone;
    m_submeshes -= static_cast<uint32_t>(g.mesh->submeshes().size());
    if (e.bounds.max.y >= m_top) m_topDirty = true;
    e = Entry{};
    m_free.push_back(h);
    if (--g.count == 0) releaseGroup(g);
}

void StaticBatcher::setTransform(Handle h, const glm::mat4& world)
{
    if (h >= m_entries.size() || !m_entries[h].group) return;
    Entry& e = m_entries[h];
    if (e.world == world) return;
    eraseGrid(h);
    if (e.bounds.max.y >= m_top) m_topDirty = true;
    e.world = world;
    e.bounds = e.group->mesh->bounds().transformed(world);
    if (!m_topDirty) m_top = std::max(m_top, e.bounds.max.y);
    e.group->instances->set(e.instance, world);
    insertGrid(h);
}

bool StaticBatcher::matches(Handle h, const void* source, const Mesh* mesh,
                            const std::vector<std::shared_ptr<Material>>& materials, bool castShadows) const
{
    if (h >= m_entries.size() || !m_entries[h].group || m_entries[h].source != source) return false;
    const Group& g = *m_entries[h].group;
    if (g.mesh.get() != mesh || g.castShadows != castShadows || g.materials.size() != materials.size()) return false;
    for (size_t i = 0; i < materials.size(); ++i) {
        if (g.materials[i] != materials[i]) return false;
    }
    return true;
}

void StaticBatcher::clear()
{
    m_entries.clear();
    m_free.clear();
    m_groupByKey.clear();
    m_cells.clear();
    m_large.clear();
    m_submeshes = 0;
    m_topDirty = true;
}

void StaticBatcher::setHidden(bool hidden)
{
    if (hidden == m_hidden) return;
    m_hidden = hidden;
    for (auto& [key, g] : m_groupByKey) g->instances->setHidden(hidden);
}

float StaticBatcher::top() const
{
    if (m_topDirty) {
        // Only after the highest entry moved or went: recompute.
        m_top = -FLT_MAX;
        for (const Entry& e : m_entries) {
            if (e.group) m_top = std::max(m_top, e.bounds.max.y);
        }
        m_topDirty = false;
    }
    return m_top;
}

// -----------------------------------------------------------------------------
// Grid
// -----------------------------------------------------------------------------

namespace {
struct CellRange {
    int x0, z0, x1, z1;
};
CellRange cellsOf(const AABB& b, float cell)
{
    return {int(std::floor(b.min.x / cell)), int(std::floor(b.min.z / cell)), int(std::floor(b.max.x / cell)),
            int(std::floor(b.max.z / cell))};
}
} // namespace

void StaticBatcher::insertGrid(Handle h)
{
    Entry& e = m_entries[h];
    const CellRange r = cellsOf(e.bounds, kCellSize);
    e.large = r.x1 - r.x0 >= kMaxCellSpan || r.z1 - r.z0 >= kMaxCellSpan;
    if (e.large) {
        m_large.push_back(h);
        return;
    }
    for (int x = r.x0; x <= r.x1; ++x)
        for (int z = r.z0; z <= r.z1; ++z) m_cells[cellKey(x, z)].push_back(h);
}

void StaticBatcher::eraseGrid(Handle h)
{
    const Entry& e = m_entries[h];
    if (e.large) {
        std::erase(m_large, h);
        return;
    }
    const CellRange r = cellsOf(e.bounds, kCellSize);
    for (int x = r.x0; x <= r.x1; ++x)
        for (int z = r.z0; z <= r.z1; ++z) {
            auto it = m_cells.find(cellKey(x, z));
            if (it == m_cells.end()) continue;
            // Keep the cell's order (the point-shadow face hash sees it).
            std::erase(it->second, h);
            if (it->second.empty()) m_cells.erase(it);
        }
}

void StaticBatcher::appendPointShadowCasters(const glm::vec3& center, float range, uint64_t stamp, Material* fallback,
                                             std::vector<DrawItem>& out)
{
    auto visit = [&](Handle h) {
        Entry& e = m_entries[h];
        if (e.stamp == stamp || !e.group->castShadows) return;
        // (The same test ShadowPass makes per item: the box's closest point within range.)
        const glm::vec3 closest = glm::clamp(center, e.bounds.min, e.bounds.max);
        if (glm::length(closest - center) > range) return;
        e.stamp = stamp;
        const Group& g = *e.group;
        for (const Submesh& sm : g.mesh->submeshes()) {
            Material* mat = g.materials.empty() ? nullptr : g.materials[std::min<size_t>(sm.materialSlot, g.materials.size() - 1)].get();
            if (!mat) mat = fallback;
            DrawItem item;
            item.mesh = g.mesh.get();
            item.submesh = &sm;
            item.material = mat;
            item.world = e.world;
            item.prevWorld = e.world;
            item.bounds = sm.bounds.transformed(e.world);
            item.castShadows = true;
            item.instanced = true;
            item.owner = e.owner;
            out.push_back(item);
        }
    };
    const AABB sphere{center - glm::vec3(range), center + glm::vec3(range)};
    const CellRange r = cellsOf(sphere, kCellSize);
    for (int x = r.x0; x <= r.x1; ++x)
        for (int z = r.z0; z <= r.z1; ++z) {
            auto it = m_cells.find(cellKey(x, z));
            if (it == m_cells.end()) continue;
            for (Handle h : it->second) visit(h);
        }
    for (Handle h : m_large) visit(h);
}

} // namespace ve
