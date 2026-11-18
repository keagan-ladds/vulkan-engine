#include "ve/world/Foliage.hpp"

#include "ve/core/Engine.hpp"
#include "ve/core/Log.hpp"
#include "ve/core/Profiler.hpp"
#include "ve/ui/UiLayer.hpp"
#include "ve/render/Renderer.hpp"
#include "ve/scene/Components.hpp"
#include "ve/scene/Scene.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <numbers>

namespace ve {

namespace {

uint32_t pcg(uint32_t v)
{
    const uint32_t state = v * 747796405u + 2891336453u;
    const uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

uint32_t hash4(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    return pcg(a ^ pcg(b ^ pcg(c ^ pcg(d))));
}

float unit(uint32_t h) { return static_cast<float>(h >> 8) * (1.0f / 16777216.0f); }

// Smooth 2D value noise in [0,1], 2 octaves.
float valueNoise(glm::vec2 p, uint32_t seed)
{
    auto lattice = [&](int x, int z) { return unit(hash4(seed, 0x51ED27u, uint32_t(x), uint32_t(z))); };
    auto octave = [&](glm::vec2 q) {
        const glm::vec2 i = glm::floor(q);
        glm::vec2 f = q - i;
        f = f * f * (3.0f - 2.0f * f);
        const int x = int(i.x), z = int(i.y);
        return glm::mix(glm::mix(lattice(x, z), lattice(x + 1, z), f.x),
                        glm::mix(lattice(x, z + 1), lattice(x + 1, z + 1), f.x), f.y);
    };
    return octave(p) * 0.67f + octave(p * 2.03f + 17.0f) * 0.33f;
}

} // namespace

FoliageSystem::FoliageSystem(FoliageWorld world, FoliageSettings settings)
    : GameObject("Foliage"), m_world(std::move(world)), m_settings(settings)
{
    if (!m_world.height) throw std::runtime_error("FoliageWorld::height is required");
    if (!m_world.normal) {
        auto height = m_world.height;
        m_world.normal = [height](glm::vec2 p) {
            const float e = 0.5f;
            const float dx = height(p + glm::vec2(e, 0)) - height(p - glm::vec2(e, 0));
            const float dz = height(p + glm::vec2(0, e)) - height(p - glm::vec2(0, e));
            return glm::normalize(glm::vec3(-dx, 2.0f * e, -dz));
        };
    }
}

FoliageSystem::~FoliageSystem()
{
    {
        std::lock_guard lock(m_mutex);
        m_quit = true;
    }
    m_cv.notify_all();
    for (std::thread& w : m_workers) w.join();
    for (auto& [key, cell] : m_cells) destroyBodies(cell);
}

uint32_t FoliageSystem::addLayer(FoliageLayer layer)
{
    if (layer.variants.empty()) throw std::runtime_error("FoliageLayer '" + layer.name + "' has no variants");
    if (layer.spacing <= 0.0f) throw std::runtime_error("FoliageLayer::spacing must be > 0");
    for (const auto& a : layer.avoid) {
        if (a.layer >= m_layers.size()) throw std::runtime_error("FoliageLayer::avoid must name an earlier layer");
    }
    if (layer.seed == 0) layer.seed = 0x9E3779B9u * static_cast<uint32_t>(m_layers.size() + 1);
    // Groups first: if they can't be created (instance buffer full), nothing
    // half-built is left in m_layers.
    LayerData data{std::move(layer), {}, 0};
    ensureGroups(data);
    m_layers.push_back(std::move(data));
    m_rescan = true;
    return static_cast<uint32_t>(m_layers.size() - 1);
}

float FoliageExclusionShape::distance(glm::vec2 p) const
{
    if (!box) return glm::length(p - center) - radius;
    const glm::vec2 d = p - center;
    const float c = std::cos(yaw), s = std::sin(yaw);
    // Into the box's frame (yaw turns local x towards -z, like a Transform rotation about Y).
    const glm::vec2 local(d.x * c - d.y * s, d.x * s + d.y * c);
    const glm::vec2 q = glm::abs(local) - halfExtents;
    return glm::length(glm::max(q, glm::vec2(0.0f))) + std::min(std::max(q.x, q.y), 0.0f);
}

glm::vec4 FoliageExclusionShape::bounds() const
{
    const float r = box ? glm::length(halfExtents) : radius;
    return {center.x - r, center.y - r, center.x + r, center.y + r};
}

void FoliageSystem::addExclusion(glm::vec2 center, float radius)
{
    FoliageExclusionShape e;
    e.center = center;
    e.radius = radius;
    m_exclusions.push_back(e);
}

void FoliageSystem::addExclusion(const FoliageExclusionShape& shape)
{
    m_exclusions.push_back(shape);
}

void FoliageSystem::setExclusions(std::vector<FoliageExclusionShape> shapes, glm::vec4 area)
{
    stopWorkers(); // they read the exclusions
    m_exclusions = std::move(shapes);
    replaceCells(area);
    startWorker();
}

void FoliageSystem::ensureGroups(LayerData& layer)
{
    const FoliageLayer& d = layer.desc;
    uint32_t capacity = d.maxInstances;
    if (capacity == 0) {
        // Everything in the loaded disc at full density, capped.
        const float r = d.viewDistance * m_settings.unloadMargin + m_settings.cellSize * 1.5f;
        const double area = std::numbers::pi * double(r) * double(r);
        const double perCell = std::clamp(double(d.probability), 0.0, 1.0) / (double(d.spacing) * d.spacing);
        capacity = static_cast<uint32_t>(std::min(area * perCell, 60000.0));
    }
    const uint32_t perVariant = capacity / uint32_t(d.variants.size()) + capacity / uint32_t(d.variants.size()) / 4 + 64;
    layer.capacity = perVariant;
    layer.groups.assign(1 + d.states.size(), {});
    for (const auto& chain : d.variants) {
        InstanceGroupDesc g;
        g.name = d.name;
        g.lods = chain;
        g.castShadows = d.castShadows;
        g.cullDistance = d.viewDistance;
        g.shadowDistance = d.shadowDistance;
        layer.groups[0].push_back(renderer().createInstanceGroup(g, perVariant));
    }
}

InstanceGroup* FoliageSystem::groupFor(LayerData& layer, uint32_t state, uint32_t variant, int* index)
{
    *index = -1;
    if (state == kFoliageRemoved || state >= layer.groups.size()) return nullptr;
    const auto& chains = state == 0 ? layer.desc.variants : layer.desc.states[state - 1].variants;
    if (chains.empty()) return nullptr; // an invisible state
    auto& groups = layer.groups[state];
    if (groups.empty()) {
        // Extra states' groups appear when first needed (most never are).
        const uint32_t cap = std::max(layer.capacity / 4, 256u);
        for (const auto& chain : chains) {
            InstanceGroupDesc g;
            g.name = layer.desc.name + " (" + layer.desc.states[state - 1].name + ")";
            g.lods = chain;
            g.castShadows = layer.desc.castShadows;
            g.cullDistance = layer.desc.viewDistance;
            g.shadowDistance = layer.desc.shadowDistance;
            groups.push_back(renderer().createInstanceGroup(g, cap));
        }
    }
    *index = int(variant % groups.size());
    return groups[size_t(*index)].get();
}

const std::vector<Collider>& FoliageSystem::collidersFor(const LayerData& layer, uint32_t state) const
{
    static const std::vector<Collider> none;
    if (state == 0) return layer.desc.colliders;
    if (state == kFoliageRemoved || state > layer.desc.states.size()) return none;
    return layer.desc.states[state - 1].colliders;
}

uint64_t FoliageSystem::cellKey(uint32_t layer, int cx, int cz)
{
    return (uint64_t(layer & 0xFFFFu) << 48) | (uint64_t(uint32_t(cx) & 0xFFFFFFu) << 24) | uint64_t(uint32_t(cz) & 0xFFFFFFu);
}

// -----------------------------------------------------------------------------
// Placement (pure functions of the layer seed and grid position)
// -----------------------------------------------------------------------------

bool FoliageSystem::candidate(uint32_t layerIndex, int gx, int gz, Candidate* out) const
{
    const FoliageLayer& L = m_layers[layerIndex].desc;
    auto rnd = [&](uint32_t k) { return unit(hash4(L.seed, uint32_t(gx), uint32_t(gz), k)); };

    if (rnd(0) >= L.probability) return false;
    const glm::vec2 p = (glm::vec2(float(gx), float(gz)) + 0.5f +
                         (glm::vec2(rnd(1), rnd(2)) - 0.5f) * L.jitter) * L.spacing;
    if (p.x < m_world.boundsMin.x || p.y < m_world.boundsMin.y || p.x > m_world.boundsMax.x || p.y > m_world.boundsMax.y)
        return false;
    for (const FoliageExclusionShape& e : m_exclusions)
        if (e.distance(p) < 0.0f) return false;
    if (L.clusterFrequency > 0.0f) {
        const float n = valueNoise(p * L.clusterFrequency, L.seed);
        const float edge = 1.0f - L.clusterCoverage;
        const float t = std::clamp((n - edge + 0.08f) / 0.16f, 0.0f, 1.0f);
        if (rnd(3) >= t * t * (3.0f - 2.0f * t)) return false;
    }
    const float h = m_world.height(p);
    if (h < L.minHeight || h > L.maxHeight) return false;
    const glm::vec3 n = m_world.normal(p);
    if (1.0f - n.y > L.maxSlope) return false;
    if (L.density && rnd(4) >= L.density(FoliageSample{p, h, n})) return false;

    for (const auto& a : L.avoid) {
        const float s = m_layers[a.layer].desc.spacing;
        const int reach = int(std::ceil(a.radius / s)) + 1;
        const int ox = int(std::floor(p.x / s)), oz = int(std::floor(p.y / s));
        for (int z = oz - reach; z <= oz + reach; ++z) {
            for (int x = ox - reach; x <= ox + reach; ++x) {
                // Cheap pre-check on the jittered position before evaluating the rules.
                const FoliageLayer& A = m_layers[a.layer].desc;
                auto ar = [&](uint32_t k) { return unit(hash4(A.seed, uint32_t(x), uint32_t(z), k)); };
                const glm::vec2 q = (glm::vec2(float(x), float(z)) + 0.5f + (glm::vec2(ar(1), ar(2)) - 0.5f) * A.jitter) * s;
                if (glm::dot(q - p, q - p) >= a.radius * a.radius) continue;
                if (candidate(a.layer, x, z, nullptr)) return false;
            }
        }
    }

    if (out) {
        out->p = p;
        out->h = h;
        out->n = n;
        out->variant = hash4(L.seed, uint32_t(gx), uint32_t(gz), 5) % uint32_t(L.variants.size());
        out->scale = glm::mix(L.minScale, L.maxScale, rnd(6));
        out->yaw = rnd(7) * 2.0f * std::numbers::pi_v<float>;
        out->tint = 1.0f + (rnd(8) * 2.0f - 1.0f) * L.tint;
    }
    return true;
}

std::vector<FoliageSystem::Instance> FoliageSystem::generate(uint32_t layerIndex, int cx, int cz) const
{
    const FoliageLayer& L = m_layers[layerIndex].desc;
    const float cs = m_settings.cellSize;
    // Grid points whose (unjittered) square starts inside this cell belong to it.
    const int gx0 = int(std::ceil(float(cx) * cs / L.spacing)), gx1 = int(std::ceil(float(cx + 1) * cs / L.spacing));
    const int gz0 = int(std::ceil(float(cz) * cs / L.spacing)), gz1 = int(std::ceil(float(cz + 1) * cs / L.spacing));
    std::vector<Instance> out;
    Instance inst;
    for (int gz = gz0; gz < gz1; ++gz)
        for (int gx = gx0; gx < gx1; ++gx)
            if (makeInstance(layerIndex, gx, gz, &inst)) out.push_back(inst);
    return out;
}

bool FoliageSystem::makeInstance(uint32_t layerIndex, int gx, int gz, Instance* out) const
{
    const FoliageLayer& L = m_layers[layerIndex].desc;
    Candidate c;
    if (!candidate(layerIndex, gx, gz, &c)) return false;
    glm::quat rot = glm::angleAxis(c.yaw, glm::vec3(0, 1, 0));
    if (L.alignToNormal > 0.0f) {
        const glm::vec3 up = glm::normalize(glm::mix(glm::vec3(0, 1, 0), c.n, L.alignToNormal));
        rot = glm::rotation(glm::vec3(0, 1, 0), up) * rot;
    }
    *out = {gx, gz, c.variant, glm::vec3(c.p.x, c.h - L.sink * c.scale, c.p.y), rot, c.scale,
            glm::vec4(c.tint, c.tint * (1.0f + (c.tint - 1.0f) * 0.3f), c.tint, 1.0f)};
    return true;
}

int FoliageSystem::cellOf(uint32_t layer, int g) const
{
    // generate() gives cell c the squares ceil(c * cs / s) .. ceil((c + 1) * cs / s) - 1.
    const float cs = m_settings.cellSize, s = m_layers[layer].desc.spacing;
    int c = int(std::floor(float(g) * s / cs));
    while (int(std::ceil(float(c) * cs / s)) > g) --c;
    while (int(std::ceil(float(c + 1) * cs / s)) <= g) ++c;
    return c;
}

std::vector<glm::vec3> FoliageSystem::query(uint32_t layer, glm::vec2 p, float radius) const
{
    std::vector<glm::vec3> out;
    if (layer >= m_layers.size()) return out;
    const float s = m_layers[layer].desc.spacing;
    const int reach = int(std::ceil(radius / s)) + 1;
    const int ox = int(std::floor(p.x / s)), oz = int(std::floor(p.y / s));
    Candidate c;
    for (int z = oz - reach; z <= oz + reach; ++z) {
        for (int x = ox - reach; x <= ox + reach; ++x) {
            if (candidate(layer, x, z, &c) && glm::length(c.p - p) <= radius) out.emplace_back(c.p.x, c.h, c.p.y);
        }
    }
    return out;
}

// -----------------------------------------------------------------------------
// Streaming
// -----------------------------------------------------------------------------

float FoliageSystem::cellDistance(int cx, int cz, glm::vec2 p) const
{
    const float cs = m_settings.cellSize;
    const glm::vec2 lo(float(cx) * cs, float(cz) * cs);
    const glm::vec2 d = glm::max(glm::max(lo - p, p - (lo + cs)), glm::vec2(0.0f));
    return glm::length(d);
}

glm::ivec4 FoliageSystem::cellRange(int x0, int z0, int x1, int z1) const
{
    // Clamp to the cells that overlap the world bounds (huge view distances stay cheap).
    const float cs = m_settings.cellSize;
    const glm::vec2 lo = glm::max(m_world.boundsMin, glm::vec2(-1e8f)) / cs;
    const glm::vec2 hi = glm::min(m_world.boundsMax, glm::vec2(1e8f)) / cs;
    return {std::max(x0, int(std::floor(lo.x)) - 1), std::max(z0, int(std::floor(lo.y)) - 1),
            std::min(x1, int(std::floor(hi.x)) + 1), std::min(z1, int(std::floor(hi.y)) + 1)};
}

bool FoliageSystem::cellInBounds(int cx, int cz) const
{
    const float cs = m_settings.cellSize;
    const glm::vec2 lo(float(cx) * cs, float(cz) * cs);
    return lo.x <= m_world.boundsMax.x && lo.y <= m_world.boundsMax.y && lo.x + cs >= m_world.boundsMin.x &&
           lo.y + cs >= m_world.boundsMin.y;
}

glm::vec3 FoliageSystem::viewerPosition() const
{
    if (Camera* cam = scene().mainCamera()) return cam->transform().position();
    return glm::vec3(0.0f);
}

glm::vec3 FoliageSystem::focusPosition(const glm::vec3& viewer) const
{
    return colliderFocus ? colliderFocus->transform().position() : viewer;
}

void FoliageSystem::refresh(FoliageWorld world, const std::vector<std::function<float(const FoliageSample&)>>& densities,
                            glm::vec4 area)
{
    // The worker reads the world and the layers' callbacks: stop it while they change.
    stopWorkers();
    m_world = std::move(world);
    for (size_t i = 0; i < m_layers.size(); ++i)
        if (i < densities.size() && densities[i]) m_layers[i].desc.density = densities[i];
    replaceCells(area);
    startWorker();
}

void FoliageSystem::stopWorkers()
{
    {
        std::lock_guard lock(m_mutex);
        m_quit = true;
    }
    m_cv.notify_all();
    for (std::thread& w : m_workers) w.join();
    m_workers.clear();
    m_quit = false;
    m_jobs.clear();
    m_results.clear();
}

void FoliageSystem::replaceCells(glm::vec4 area)
{
    float margin = 0.0f; // placement looks this far around (spacing, avoid distances)
    for (const LayerData& layer : m_layers) {
        const FoliageLayer& L = layer.desc;
        float reach = L.spacing;
        for (const auto& a : L.avoid) reach = std::max(reach, a.radius + m_layers[a.layer].desc.spacing);
        margin = std::max(margin, reach);
    }
    // Cells near the area are placed again; so are queued ones (their jobs are gone).
    const float cs = m_settings.cellSize;
    for (auto it = m_cells.begin(); it != m_cells.end();) {
        Cell& c = it->second;
        const glm::vec2 lo(float(c.cx) * cs, float(c.cz) * cs);
        const bool near = lo.x - margin <= area.z && lo.x + cs + margin >= area.x && lo.y - margin <= area.w && lo.y + cs + margin >= area.y;
        if (near || c.state == CellState::Queued) {
            unload(c);
            it = m_cells.erase(it);
        } else {
            ++it;
        }
    }
    m_rescan = true;
}

float FoliageSystem::streamProgress() const
{
    if (m_scanViewer.x > 1e29f) return 0.0f; // not scanned yet
    size_t done = 0, total = 0;
    const float ready = m_settings.readyDistance;
    for (const auto& [key, cell] : m_cells) {
        if (ready > 0.0f && cellDistance(cell.cx, cell.cz, m_scanViewer) > ready) continue;
        ++total;
        done += cell.state == CellState::Loaded ? 1 : 0;
    }
    return total == 0 ? 1.0f : float(done) / float(total);
}

void FoliageSystem::startWorker()
{
    if (!m_workers.empty()) return;
    // Placement is independent per cell: one worker per spare core.
    const unsigned n = std::max(1u, std::min(std::thread::hardware_concurrency(), 16u) - 1u);
    for (unsigned i = 0; i < n; ++i) m_workers.emplace_back([this] { workerLoop(); });
}

void FoliageSystem::workerLoop()
{
    for (;;) {
        Job job;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [&] { return m_quit || !m_jobs.empty(); });
            if (m_quit) return;
            // Nearest job first.
            auto best = m_jobs.begin();
            float bestDist = FLT_MAX;
            for (auto it = m_jobs.begin(); it != m_jobs.end(); ++it) {
                const float d = cellDistance(it->cx, it->cz, m_workerViewer);
                if (d < bestDist) {
                    bestDist = d;
                    best = it;
                }
            }
            job = *best;
            *best = m_jobs.back();
            m_jobs.pop_back();
        }
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<Instance> instances = generate(job.layer, job.cx, job.cz);
        const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::lock_guard lock(m_mutex);
        m_results.push_back({job.key, job.ticket, std::move(instances), ms});
    }
}

size_t FoliageSystem::applyInstances(Cell& cell, size_t budget)
{
    size_t added = 0;
    cell.slots.resize(cell.instances.size());
    while (cell.applied < cell.instances.size() && added < budget) {
        const size_t i = cell.applied++;
        const Instance& inst = cell.instances[i];
        // The game may have changed this instance (felled, harvested): re-apply it.
        const auto it = m_overrides.find(FoliageInstanceId{cell.layer, inst.gx, inst.gz}.packed());
        cell.slots[i].state = it != m_overrides.end() ? it->second : 0u;
        showInstance(cell, i);
        ++added;
    }
    if (cell.applied == cell.instances.size()) cell.state = CellState::Loaded;
    return added;
}

void FoliageSystem::showInstance(Cell& cell, size_t i)
{
    LayerData& layer = m_layers[cell.layer];
    Slot& slot = cell.slots[i];
    if (slot.group >= 0 || slot.promoted) return;
    const Instance& inst = cell.instances[i];
    int index;
    InstanceGroup* g = groupFor(layer, slot.state, inst.variant, &index);
    if (!g) return; // removed / invisible state
    if (g->count() >= g->capacity()) {
        ++m_stats.dropped;
        if (layer.capacityWarned++ == 0)
            log::warn("Foliage layer '{}' is full ({} per variant); raise FoliageLayer::maxInstances", layer.desc.name,
                      g->capacity());
        return;
    }
    const glm::mat4 m = glm::translate(glm::mat4(1.0f), inst.position) * glm::mat4_cast(inst.rotation) *
                        glm::scale(glm::mat4(1.0f), glm::vec3(inst.scale));
    slot.group = index;
    slot.handle = g->add(m, inst.custom);
}

void FoliageSystem::hideInstance(Cell& cell, size_t i)
{
    Slot& slot = cell.slots[i];
    if (slot.group < 0) return;
    m_layers[cell.layer].groups[slot.state][size_t(slot.group)]->remove(slot.handle);
    slot.group = -1;
    slot.handle = InstanceGroup::kInvalid;
}

void FoliageSystem::createBody(Cell& cell, size_t i)
{
    Slot& slot = cell.slots[i];
    if (slot.hasBody || slot.promoted) return;
    const Instance& inst = cell.instances[i];
    const auto& colliders = collidersFor(m_layers[cell.layer], slot.state);
    if (colliders.empty()) return;
    const Collider& c = colliders[colliders.size() == 1 ? 0 : inst.variant % colliders.size()];
    slot.body = physics().createStaticBody(c, inst.position, inst.rotation, glm::vec3(inst.scale),
                                           m_layers[cell.layer].desc.colliderUserValue, m_layers[cell.layer].desc.colliderSurface);
    slot.hasBody = true;
    ++cell.bodies;
}

void FoliageSystem::destroyBody(Cell& cell, size_t i)
{
    Slot& slot = cell.slots[i];
    if (!slot.hasBody) return;
    physics().destroyStaticBody(slot.body);
    slot.hasBody = false;
    --cell.bodies;
}

void FoliageSystem::destroyBodies(Cell& cell)
{
    for (size_t i = 0; i < cell.slots.size(); ++i) destroyBody(cell, i);
    cell.bodiesCreated = 0;
}

void FoliageSystem::unload(Cell& cell)
{
    for (size_t i = 0; i < cell.slots.size(); ++i) {
        if (cell.slots[i].promoted) demote(cell, i);
        hideInstance(cell, i);
    }
    destroyBodies(cell);
}

void FoliageSystem::preload(const glm::vec3& viewer)
{
    const glm::vec2 v(viewer.x, viewer.z);
    const glm::vec3 focus = focusPosition(viewer);
    const auto t0 = std::chrono::steady_clock::now();
    size_t count = 0;
    for (uint32_t li = 0; li < m_layers.size(); ++li) {
        const FoliageLayer& L = m_layers[li].desc;
        const float cs = m_settings.cellSize;
        const int r = int(std::ceil(L.viewDistance / cs)) + 1;
        const int vx = int(std::floor(v.x / cs)), vz = int(std::floor(v.y / cs));
        const glm::ivec4 range = cellRange(vx - r, vz - r, vx + r, vz + r);
        for (int cz = range.y; cz <= range.w; ++cz) {
            for (int cx = range.x; cx <= range.z; ++cx) {
                if (cellDistance(cx, cz, v) > L.viewDistance || !cellInBounds(cx, cz)) continue;
                const uint64_t key = cellKey(li, cx, cz);
                if (m_cells.count(key)) continue;
                Cell cell{li, cx, cz, m_nextTicket++};
                cell.instances = generate(li, cx, cz);
                applyInstances(cell, SIZE_MAX);
                count += cell.instances.size();
                if (cellDistance(cx, cz, glm::vec2(focus.x, focus.z)) < L.colliderDistance) {
                    cell.wantBodies = true;
                }
                m_cells.emplace(key, std::move(cell));
            }
        }
    }
    // Colliders without a budget too.
    for (auto& [key, cell] : m_cells) {
        if (!cell.wantBodies) continue;
        for (size_t i = 0; i < cell.instances.size(); ++i) createBody(cell, i);
        cell.bodiesCreated = cell.instances.size();
    }
    m_rescan = true; // the next update() walks the (new) cell set
    std::string perLayer;
    for (const auto& layer : m_layers) {
        uint32_t n = 0;
        for (const auto& g : layer.groups[0]) n += g->count();
        perLayer += std::format("{}{} {}", perLayer.empty() ? "" : ", ", n, layer.desc.name);
    }
    log::info("Foliage preload: {} instances in {} cells ({}), {:.0f} ms", count, m_cells.size(), perLayer,
              std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count());
}

void FoliageSystem::update(float)
{
    VE_PROFILE("Foliage streaming");
    startWorker();
    const glm::vec3 viewer3 = viewerPosition();
    const glm::vec2 viewer(viewer3.x, viewer3.z);
    const glm::vec3 focus3 = focusPosition(viewer3);
    const glm::vec2 focus(focus3.x, focus3.z);
    const float cs = m_settings.cellSize;

    // 1-2 only when the viewer moved (a quarter cell) or the cell set changed.
    const bool rescan = m_rescan || glm::distance(viewer, m_scanViewer) > cs * 0.25f;
    if (rescan) {
        m_scanViewer = viewer;
        m_rescan = false;
    }
    bool unloaded = false;

    // 1. Unload cells that fell behind (or cancel their jobs: the ticket goes stale).
    for (auto it = rescan ? m_cells.begin() : m_cells.end(); it != m_cells.end();) {
        const FoliageLayer& L = m_layers[it->second.layer].desc;
        if (cellDistance(it->second.cx, it->second.cz, viewer) > L.viewDistance * m_settings.unloadMargin) {
            unload(it->second);
            it = m_cells.erase(it);
            unloaded = true;
        } else {
            ++it;
        }
    }

    // 2. Request cells that came into range.
    std::vector<Job> newJobs;
    for (uint32_t li = 0; li < (rescan ? m_layers.size() : 0); ++li) {
        const FoliageLayer& L = m_layers[li].desc;
        const int r = int(std::ceil(L.viewDistance / cs)) + 1;
        const int vx = int(std::floor(viewer.x / cs)), vz = int(std::floor(viewer.y / cs));
        const glm::ivec4 range = cellRange(vx - r, vz - r, vx + r, vz + r);
        for (int cz = range.y; cz <= range.w; ++cz) {
            for (int cx = range.x; cx <= range.z; ++cx) {
                if (cellDistance(cx, cz, viewer) > L.viewDistance || !cellInBounds(cx, cz)) continue;
                const uint64_t key = cellKey(li, cx, cz);
                if (m_cells.count(key)) continue;
                const uint32_t ticket = m_nextTicket++;
                m_cells.emplace(key, Cell{li, cx, cz, ticket});
                newJobs.push_back({key, ticket, li, cx, cz});
            }
        }
    }

    // 3. Hand jobs to the worker, take its results.
    std::vector<Result> results;
    {
        std::lock_guard lock(m_mutex);
        m_workerViewer = viewer;
        // Drop queued jobs whose cell was unloaded meanwhile.
        if (unloaded) {
            std::erase_if(m_jobs, [&](const Job& j) {
                auto it = m_cells.find(j.key);
                return it == m_cells.end() || it->second.ticket != j.ticket;
            });
        }
        m_jobs.insert(m_jobs.end(), newJobs.begin(), newJobs.end());
        results.swap(m_results);
        m_stats.pending = static_cast<uint32_t>(m_jobs.size());
    }
    if (!newJobs.empty()) m_cv.notify_all();
    for (Result& r : results) {
        m_stats.generateMs = m_stats.generateMs * 0.95f + r.ms * 0.05f;
        auto it = m_cells.find(r.key);
        if (it == m_cells.end() || it->second.ticket != r.ticket) continue; // stale
        it->second.instances = std::move(r.instances);
        it->second.state = CellState::Ready;
        m_readyWaiting = true;
    }

    // 4. Add ready instances to the GPU groups (budgeted, nearest first).
    std::vector<Cell*> ready;
    if (m_readyWaiting) {
        for (auto& [key, cell] : m_cells) {
            if (cell.state == CellState::Ready) ready.push_back(&cell);
        }
    }
    std::sort(ready.begin(), ready.end(), [&](const Cell* a, const Cell* b) {
        return cellDistance(a->cx, a->cz, viewer) < cellDistance(b->cx, b->cz, viewer);
    });
    size_t budget = m_settings.instancesPerFrame;
    for (Cell* cell : ready) {
        if (budget == 0) break;
        budget -= applyInstances(*cell, budget);
    }
    const bool applied = budget < m_settings.instancesPerFrame;
    m_readyWaiting = !ready.empty() && budget == 0;
    updatePromotion(focus);

    // 5. Colliders near the focus (budgeted); dropped with some hysteresis.
    //    Only when the focus moved, instances arrived/left or bodies are still owed.
    if (!(rescan || unloaded || applied || m_bodiesWaiting || glm::distance(focus, m_scanFocus) > 1.0f)) return;
    m_scanFocus = focus;
    size_t bodyBudget = m_settings.bodiesPerFrame;
    uint32_t instances = 0, bodies = 0;
    for (auto& [key, cell] : m_cells) {
        instances += static_cast<uint32_t>(cell.applied);
        const FoliageLayer& L = m_layers[cell.layer].desc;
        if ((L.colliders.empty() && L.states.empty()) || cell.state != CellState::Loaded) continue;
        const float d = cellDistance(cell.cx, cell.cz, focus);
        if (d < L.colliderDistance) cell.wantBodies = true;
        else if (d > L.colliderDistance * 1.25f + 8.0f) cell.wantBodies = false;

        if (!cell.wantBodies) {
            if (cell.bodiesCreated) destroyBodies(cell);
        } else {
            while (cell.bodiesCreated < cell.instances.size() && bodyBudget > 0) {
                createBody(cell, cell.bodiesCreated++);
                --bodyBudget;
            }
        }
        bodies += cell.bodies;
    }
    m_bodiesWaiting = bodyBudget == 0;
    m_stats.cells = static_cast<uint32_t>(m_cells.size());
    m_stats.instances = instances;
    m_stats.bodies = bodies;
}

// -----------------------------------------------------------------------------
// Interaction: per-instance state and promotion to GameObjects
// -----------------------------------------------------------------------------

std::optional<FoliageInstanceInfo> FoliageSystem::instance(FoliageInstanceId id) const
{
    Instance inst;
    if (!id.valid() || id.layer >= m_layers.size() || !makeInstance(id.layer, id.gx, id.gz, &inst)) return std::nullopt;
    return FoliageInstanceInfo{id, inst.position, inst.rotation, inst.scale, inst.variant, state(id)};
}

std::optional<FoliageInstanceInfo> FoliageSystem::nearest(uint32_t layer, glm::vec2 p, float radius) const
{
    if (layer >= m_layers.size()) return std::nullopt;
    const float s = m_layers[layer].desc.spacing;
    const int reach = int(std::ceil(radius / s)) + 1;
    const int ox = int(std::floor(p.x / s)), oz = int(std::floor(p.y / s));
    std::optional<FoliageInstanceInfo> best;
    float bestDist = radius;
    Instance inst;
    for (int z = oz - reach; z <= oz + reach; ++z) {
        for (int x = ox - reach; x <= ox + reach; ++x) {
            if (!makeInstance(layer, x, z, &inst)) continue;
            const float d = glm::length(glm::vec2(inst.position.x, inst.position.z) - p);
            if (d > bestDist) continue;
            const FoliageInstanceId id{layer, x, z};
            if (state(id) == kFoliageRemoved) continue;
            bestDist = d;
            best = FoliageInstanceInfo{id, inst.position, inst.rotation, inst.scale, inst.variant, state(id)};
        }
    }
    return best;
}

uint32_t FoliageSystem::state(FoliageInstanceId id) const
{
    const auto it = m_overrides.find(id.packed());
    return it == m_overrides.end() ? 0u : it->second;
}

int FoliageSystem::stateIndex(uint32_t layer, const std::string& name) const
{
    if (layer >= m_layers.size()) return -1;
    if (name.empty()) return 0;
    const auto& states = m_layers[layer].desc.states;
    for (size_t i = 0; i < states.size(); ++i)
        if (states[i].name == name) return int(i + 1);
    return -1;
}

const std::string& FoliageSystem::stateName(uint32_t layer, uint32_t state) const
{
    static const std::string none, removed = "Removed";
    if (state == kFoliageRemoved) return removed;
    if (layer >= m_layers.size() || state == 0 || state > m_layers[layer].desc.states.size()) return none;
    return m_layers[layer].desc.states[state - 1].name;
}

FoliageSystem::Cell* FoliageSystem::loadedCell(FoliageInstanceId id, size_t* index)
{
    if (!id.valid() || id.layer >= m_layers.size()) return nullptr;
    auto it = m_cells.find(cellKey(id.layer, cellOf(id.layer, id.gx), cellOf(id.layer, id.gz)));
    if (it == m_cells.end()) return nullptr;
    Cell& cell = it->second;
    for (size_t i = 0; i < cell.applied; ++i) {
        if (cell.instances[i].gx == id.gx && cell.instances[i].gz == id.gz) {
            *index = i;
            return &cell;
        }
    }
    return nullptr;
}

void FoliageSystem::setState(FoliageInstanceId id, uint32_t newState)
{
    if (!id.valid() || id.layer >= m_layers.size()) return;
    if (newState != kFoliageRemoved && newState > m_layers[id.layer].desc.states.size()) {
        log::warnOnce("Foliage layer '" + m_layers[id.layer].desc.name + "': no state " + std::to_string(newState));
        return;
    }
    const auto prev = m_overrides.find(id.packed());
    const uint32_t oldState = prev != m_overrides.end() ? prev->second : 0;
    if (newState == 0) m_overrides.erase(id.packed());
    else m_overrides[id.packed()] = newState;
    if (oldState != newState && !m_stateListeners.empty()) {
        Instance inst;
        if (makeInstance(id.layer, id.gx, id.gz, &inst)) {
            const auto listeners = m_stateListeners; // a listener may remove itself
            for (const auto& [lid, fn] : listeners) fn(id, inst.position);
        }
    }

    size_t i;
    Cell* cell = loadedCell(id, &i);
    if (!cell || cell->slots[i].state == newState) return;
    // A promoted instance is handed back (its object goes) and promoted again
    // next frame with the new state if the player is still close.
    if (cell->slots[i].promoted) demote(*cell, i);
    hideInstance(*cell, i);
    destroyBody(*cell, i);
    cell->slots[i].state = newState;
    showInstance(*cell, i);
    if (cell->wantBodies && i < cell->bodiesCreated) createBody(*cell, i);
}

void FoliageSystem::forEachCollider(glm::vec4 area, const std::function<void(const Collider&, const glm::mat4&)>& fn) const
{
    for (uint32_t l = 0; l < m_layers.size(); ++l) {
        const LayerData& layer = m_layers[l];
        if (layer.desc.colliders.empty() && layer.desc.states.empty()) continue;
        const float sp = layer.desc.spacing;
        // Instances stay inside their grid square: one square of margin is plenty.
        const int x0 = int(std::floor(area.x / sp)) - 1, z0 = int(std::floor(area.y / sp)) - 1;
        const int x1 = int(std::floor(area.z / sp)) + 1, z1 = int(std::floor(area.w / sp)) + 1;
        Instance inst;
        for (int gz = z0; gz <= z1; ++gz) {
            for (int gx = x0; gx <= x1; ++gx) {
                const FoliageInstanceId id{l, gx, gz};
                const auto o = m_overrides.find(id.packed());
                const uint32_t st = o != m_overrides.end() ? o->second : 0;
                const auto& colliders = collidersFor(layer, st);
                if (colliders.empty() || !makeInstance(l, gx, gz, &inst)) continue;
                if (inst.position.x < area.x || inst.position.x > area.z || inst.position.z < area.y || inst.position.z > area.w) continue;
                const Collider& c = colliders[colliders.size() == 1 ? 0 : inst.variant % colliders.size()];
                const glm::mat4 m = glm::translate(glm::mat4(1.0f), inst.position) * glm::mat4_cast(inst.rotation) *
                                    glm::scale(glm::mat4(1.0f), glm::vec3(inst.scale));
                fn(c, m);
            }
        }
    }
}

uint32_t FoliageSystem::addStateListener(StateListener fn)
{
    m_stateListeners.emplace_back(m_nextListener, std::move(fn));
    return m_nextListener++;
}

void FoliageSystem::removeStateListener(uint32_t id)
{
    std::erase_if(m_stateListeners, [id](const auto& l) { return l.first == id; });
}

std::vector<std::pair<uint64_t, uint32_t>> FoliageSystem::overrides() const
{
    return {m_overrides.begin(), m_overrides.end()};
}

void FoliageSystem::setOverrides(const std::vector<std::pair<uint64_t, uint32_t>>& states)
{
    // Back to the generated world, then apply the given changes.
    std::vector<uint64_t> changed;
    for (const auto& [k, s] : m_overrides) changed.push_back(k);
    for (uint64_t k : changed) setState(FoliageInstanceId::unpack(k), 0);
    for (const auto& [k, s] : states) setState(FoliageInstanceId::unpack(k), s);
}

GameObject* FoliageSystem::promoted(FoliageInstanceId id) const
{
    size_t i;
    Cell* cell = const_cast<FoliageSystem*>(this)->loadedCell(id, &i);
    return cell && cell->slots[i].promoted ? scene().findById(cell->slots[i].promoted) : nullptr;
}

void FoliageSystem::promote(Cell& cell, size_t i)
{
    LayerData& layer = m_layers[cell.layer];
    Slot& slot = cell.slots[i];
    if (slot.promoted || slot.state == kFoliageRemoved) return;
    const Instance& inst = cell.instances[i];
    const FoliageInstanceInfo info{{cell.layer, inst.gx, inst.gz}, inst.position, inst.rotation, inst.scale, inst.variant, slot.state};
    GameObject* obj = layer.desc.promote ? layer.desc.promote(scene(), info) : &scene().spawn(layer.desc.name);
    if (!obj) return;
    obj->transform().setPosition(inst.position);
    obj->transform().setRotation(inst.rotation);
    obj->transform().setLocalScale(glm::vec3(inst.scale));
    auto& link = obj->addComponent<FoliageLink>();
    link.info = info;
    link.m_system = id();
    // Unless the game's object brings its own, it looks and collides like the instance.
    if (!obj->getComponent<MeshRenderer>()) {
        int index;
        if (groupFor(layer, slot.state, inst.variant, &index)) {
            const auto& chains = slot.state == 0 ? layer.desc.variants : layer.desc.states[slot.state - 1].variants;
            const InstanceLod& lod0 = chains[inst.variant % chains.size()].front();
            obj->addComponent<MeshRenderer>(lod0.mesh, lod0.materials);
        }
    }
    const auto& colliders = collidersFor(layer, slot.state);
    if (!colliders.empty() && !obj->getComponent<Rigidbody>()) {
        auto& rb = obj->addComponent<Rigidbody>(colliders[colliders.size() == 1 ? 0 : inst.variant % colliders.size()],
                                                BodyType::Static);
        (void)rb;
    }
    hideInstance(cell, i);
    destroyBody(cell, i);
    slot.promoted = obj->id();
    m_promoted.push_back(info.id);
}

void FoliageSystem::demote(Cell& cell, size_t i)
{
    Slot& slot = cell.slots[i];
    if (!slot.promoted) return;
    if (GameObject* obj = scene().findById(slot.promoted)) obj->destroy();
    slot.promoted = 0;
    const Instance& inst = cell.instances[i];
    std::erase(m_promoted, FoliageInstanceId{cell.layer, inst.gx, inst.gz});
    showInstance(cell, i);
    if (cell.wantBodies && i < cell.bodiesCreated) createBody(cell, i);
}

void FoliageSystem::updatePromotion(glm::vec2 focus)
{
    // Hand back what's out of reach (25 % hysteresis) or whose object is gone.
    for (size_t k = 0; k < m_promoted.size();) {
        const FoliageInstanceId id = m_promoted[k];
        size_t i;
        Cell* cell = loadedCell(id, &i);
        const float far = m_layers[id.layer].desc.promoteDistance * 1.25f + 1.0f;
        if (!cell) {
            m_promoted.erase(m_promoted.begin() + ptrdiff_t(k));
            continue;
        }
        const Instance& inst = cell->instances[i];
        const bool gone = !scene().findById(cell->slots[i].promoted);
        if (gone || glm::length(glm::vec2(inst.position.x, inst.position.z) - focus) > far) {
            demote(*cell, i); // erases m_promoted[k]
            continue;
        }
        ++k;
    }
    // Promote what came close.
    const float cs = m_settings.cellSize;
    for (uint32_t li = 0; li < m_layers.size(); ++li) {
        const float r = m_layers[li].desc.promoteDistance;
        if (r <= 0.0f) continue;
        const int x0 = int(std::floor((focus.x - r) / cs)), x1 = int(std::floor((focus.x + r) / cs));
        const int z0 = int(std::floor((focus.y - r) / cs)), z1 = int(std::floor((focus.y + r) / cs));
        for (int cz = z0; cz <= z1; ++cz) {
            for (int cx = x0; cx <= x1; ++cx) {
                auto it = m_cells.find(cellKey(li, cx, cz));
                if (it == m_cells.end() || it->second.state != CellState::Loaded) continue;
                Cell& cell = it->second;
                for (size_t i = 0; i < cell.instances.size(); ++i) {
                    const Instance& inst = cell.instances[i];
                    if (!cell.slots[i].promoted && glm::length(glm::vec2(inst.position.x, inst.position.z) - focus) < r)
                        promote(cell, i);
                }
            }
        }
    }
}

void FoliageSystem::onDestroy()
{
    for (FoliageInstanceId id : m_promoted) {
        if (GameObject* obj = promoted(id)) obj->destroy();
    }
    m_promoted.clear();
}

// -----------------------------------------------------------------------------
// FoliageLink
// -----------------------------------------------------------------------------

FoliageSystem* FoliageLink::system() const
{
    return dynamic_cast<FoliageSystem*>(gameObject().scene().findById(m_system));
}

void FoliageLink::setState(uint32_t state)
{
    if (FoliageSystem* s = system()) s->setState(info.id, state);
}

void FoliageLink::setState(const std::string& name)
{
    FoliageSystem* s = system();
    if (!s) return;
    const int index = s->stateIndex(info.id.layer, name);
    if (index < 0) log::warnOnce("FoliageLink: no foliage state '" + name + "'");
    else s->setState(info.id, uint32_t(index));
}

std::string FoliageLink::stateName() const
{
    const FoliageSystem* s = system();
    return s ? s->stateName(info.id.layer, s->state(info.id)) : std::string();
}

void FoliageSystem::onGui()
{
    // Shown with the engine's debug overlay (F1), in its own window.
    if (!engine().ui().debugOverlay) return;
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 430.0f, 60.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Foliage")) {
        ImGui::End();
        return;
    }
    ImGui::Text("Cells: %u (%u pending), %.2f ms per cell", m_stats.cells, m_stats.pending, m_stats.generateMs);
    ImGui::Text("Instances: %u, static bodies: %u, dropped: %u", m_stats.instances, m_stats.bodies, m_stats.dropped);
    for (auto& layer : m_layers) {
        uint32_t n = 0, cap = 0;
        for (auto& states : layer.groups) {
            for (auto& g : states) {
                n += g->count();
                cap += g->capacity();
            }
        }
        ImGui::Text("  %-10s %6u / %6u  (view %.0f m)", layer.desc.name.c_str(), n, cap, layer.desc.viewDistance);
    }
    ImGui::End();
}

} // namespace ve
