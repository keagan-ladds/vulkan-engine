#include "ve/world/Foliage.hpp"

#include "ve/core/Engine.hpp"
#include "ve/core/Log.hpp"
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
    if (m_worker.joinable()) m_worker.join();
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
    m_layers.push_back({std::move(layer), {}, 0});
    ensureGroups(m_layers.back());
    m_rescan = true;
    return static_cast<uint32_t>(m_layers.size() - 1);
}

void FoliageSystem::addExclusion(glm::vec2 center, float radius) { m_exclusions.push_back({center, radius}); }

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
    for (const auto& chain : d.variants) {
        InstanceGroupDesc g;
        g.name = d.name;
        g.lods = chain;
        g.castShadows = d.castShadows;
        g.cullDistance = d.viewDistance;
        g.shadowDistance = d.shadowDistance;
        layer.groups.push_back(renderer().createInstanceGroup(g, perVariant));
    }
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
    for (const auto& [c, r] : m_exclusions) {
        if (glm::dot(p - c, p - c) < r * r) return false;
    }
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
    Candidate c;
    for (int gz = gz0; gz < gz1; ++gz) {
        for (int gx = gx0; gx < gx1; ++gx) {
            if (!candidate(layerIndex, gx, gz, &c)) continue;
            glm::quat rot = glm::angleAxis(c.yaw, glm::vec3(0, 1, 0));
            if (L.alignToNormal > 0.0f) {
                const glm::vec3 up = glm::normalize(glm::mix(glm::vec3(0, 1, 0), c.n, L.alignToNormal));
                rot = glm::rotation(glm::vec3(0, 1, 0), up) * rot;
            }
            out.push_back({c.variant, glm::vec3(c.p.x, c.h - L.sink * c.scale, c.p.y), rot, c.scale,
                           glm::vec4(c.tint, c.tint * (1.0f + (c.tint - 1.0f) * 0.3f), c.tint, 1.0f)});
        }
    }
    return out;
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

void FoliageSystem::startWorker()
{
    if (!m_worker.joinable()) m_worker = std::thread([this] { workerLoop(); });
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
    LayerData& layer = m_layers[cell.layer];
    size_t added = 0;
    while (cell.applied < cell.instances.size() && added < budget) {
        const Instance& inst = cell.instances[cell.applied++];
        InstanceGroup& g = *layer.groups[inst.variant];
        if (g.count() >= g.capacity()) {
            ++m_stats.dropped;
            if (layer.capacityWarned++ == 0)
                log::warn("Foliage layer '{}' is full ({} per variant); raise FoliageLayer::maxInstances", layer.desc.name,
                          g.capacity());
            continue;
        }
        const glm::mat4 m = glm::translate(glm::mat4(1.0f), inst.position) * glm::mat4_cast(inst.rotation) *
                            glm::scale(glm::mat4(1.0f), glm::vec3(inst.scale));
        cell.handles.push_back({inst.variant, g.add(m, inst.custom)});
        ++added;
    }
    if (cell.applied == cell.instances.size()) cell.state = CellState::Loaded;
    return added;
}

void FoliageSystem::destroyBodies(Cell& cell)
{
    for (uint32_t id : cell.bodies) physics().destroyStaticBody(id);
    cell.bodies.clear();
    cell.bodiesCreated = 0;
}

void FoliageSystem::unload(Cell& cell)
{
    LayerData& layer = m_layers[cell.layer];
    for (const auto& [variant, handle] : cell.handles) layer.groups[variant]->remove(handle);
    cell.handles.clear();
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
                if (!L.colliders.empty() && cellDistance(cx, cz, glm::vec2(focus.x, focus.z)) < L.colliderDistance) {
                    cell.wantBodies = true;
                }
                m_cells.emplace(key, std::move(cell));
            }
        }
    }
    // Colliders without a budget too.
    for (auto& [key, cell] : m_cells) {
        if (!cell.wantBodies) continue;
        const FoliageLayer& L = m_layers[cell.layer].desc;
        for (const Instance& inst : cell.instances) {
            const Collider& c = L.colliders[L.colliders.size() == 1 ? 0 : inst.variant];
            cell.bodies.push_back(physics().createStaticBody(c, inst.position, inst.rotation, glm::vec3(inst.scale),
                                                             L.colliderUserValue));
        }
        cell.bodiesCreated = cell.instances.size();
    }
    m_rescan = true; // the next update() walks the (new) cell set
    std::string perLayer;
    for (const auto& layer : m_layers) {
        uint32_t n = 0;
        for (const auto& g : layer.groups) n += g->count();
        perLayer += std::format("{}{} {}", perLayer.empty() ? "" : ", ", n, layer.desc.name);
    }
    log::info("Foliage preload: {} instances in {} cells ({}), {:.0f} ms", count, m_cells.size(), perLayer,
              std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count());
}

void FoliageSystem::update(float)
{
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
    if (!newJobs.empty()) m_cv.notify_one();
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

    // 5. Colliders near the focus (budgeted); dropped with some hysteresis.
    //    Only when the focus moved, instances arrived/left or bodies are still owed.
    if (!(rescan || unloaded || applied || m_bodiesWaiting || glm::distance(focus, m_scanFocus) > 1.0f)) return;
    m_scanFocus = focus;
    size_t bodyBudget = m_settings.bodiesPerFrame;
    uint32_t instances = 0, bodies = 0;
    for (auto& [key, cell] : m_cells) {
        instances += static_cast<uint32_t>(cell.handles.size());
        const FoliageLayer& L = m_layers[cell.layer].desc;
        if (L.colliders.empty() || cell.state != CellState::Loaded) continue;
        const float d = cellDistance(cell.cx, cell.cz, focus);
        if (d < L.colliderDistance) cell.wantBodies = true;
        else if (d > L.colliderDistance * 1.25f + 8.0f) cell.wantBodies = false;

        if (!cell.wantBodies) {
            if (!cell.bodies.empty()) destroyBodies(cell);
        } else {
            while (cell.bodiesCreated < cell.instances.size() && bodyBudget > 0) {
                const Instance& inst = cell.instances[cell.bodiesCreated++];
                const Collider& c = L.colliders[L.colliders.size() == 1 ? 0 : inst.variant];
                cell.bodies.push_back(physics().createStaticBody(c, inst.position, inst.rotation, glm::vec3(inst.scale),
                                                                 L.colliderUserValue));
                --bodyBudget;
            }
        }
        bodies += static_cast<uint32_t>(cell.bodies.size());
    }
    m_bodiesWaiting = bodyBudget == 0;
    m_stats.cells = static_cast<uint32_t>(m_cells.size());
    m_stats.instances = instances;
    m_stats.bodies = bodies;
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
        for (auto& g : layer.groups) {
            n += g->count();
            cap += g->capacity();
        }
        ImGui::Text("  %-10s %6u / %6u  (view %.0f m)", layer.desc.name.c_str(), n, cap, layer.desc.viewDistance);
    }
    ImGui::End();
}

} // namespace ve
