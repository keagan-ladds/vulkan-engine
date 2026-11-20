#include "ve/nav/Navigation.hpp"

#include "ve/core/Engine.hpp"
#include "ve/core/Log.hpp"
#include "ve/core/Profiler.hpp"
#include "ve/debug/Debug.hpp"
#include "ve/physics/Physics.hpp"
#include "ve/render/Renderer.hpp"
#include "ve/scene/Scene.hpp"
#include "ve/world/Foliage.hpp"
#include "ve/world/Terrain.hpp"

#include <DetourCommon.h>
#include <DetourCrowd.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <Recast.h>

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace ve {

namespace {

constexpr int kMaxPathPolys = 512;
constexpr int kMaxAgents = 256;
constexpr unsigned short kWalkFlag = 1;

uint32_t nextPow2(uint32_t v)
{
    uint32_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

const float* ptr(const glm::vec3& v) { return &v.x; }
glm::vec3 vec(const float* p) { return {p[0], p[1], p[2]}; }

// Tile builds are independent: a few jobs at a time on one navmesh's queue.
size_t buildJobs()
{
    return std::clamp(std::max(2u, jobs::hardwareThreads()) / 2, 1u, 4u);
}

int borderCells(const NavMeshSettings& s) { return int(std::ceil(s.agentRadius / s.cellSize)) + 3; }

// Detour wants a plain function for random numbers.
thread_local std::mt19937* t_rng = nullptr;
float frand() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(*t_rng); }

} // namespace

float NavPath::length() const
{
    float d = 0.0f;
    for (size_t i = 1; i < corners.size(); ++i) d += glm::distance(corners[i - 1], corners[i]);
    return d;
}

// =============================================================================
// NavMeshSurface
// =============================================================================

NavMeshSurface::NavMeshSurface() = default;

NavMeshSurface::~NavMeshSurface()
{
    destroyNav();
}

NavMeshSurface* NavMeshSurface::find(const Scene& scene)
{
    NavMeshSurface* found = nullptr;
    scene.forEach<NavMeshSurface>([&](NavMeshSurface& s) {
        if (!found && s.isActive()) found = &s;
    });
    return found;
}

void NavMeshSurface::destroyNav()
{
    // Tile builds only read their own TileJob: drop them, a build in flight
    // finishes into the old queue (freed with it) without us waiting.
    if (m_queue) {
        std::lock_guard lock(m_queue->mutex);
        m_queue->quit = true;
        m_queue->jobs.clear();
    }
    m_drains.cancel();
    m_queue.reset();
    if (m_crowd) dtFreeCrowd(m_crowd);
    if (m_query) dtFreeNavMeshQuery(m_query);
    if (m_nav) dtFreeNavMesh(m_nav);
    m_crowd = nullptr;
    m_query = nullptr;
    m_nav = nullptr;
    m_tileVersion.clear();
    m_tilesBuilt.clear();
    m_tilesReady = 0;
    m_stats = {};
}

void NavMeshSurface::start()
{
    if (!buildOnStart) return;
    m_buildRequested = true;
    // Behind a loading screen: build there (world updates are paused, so the
    // task itself drives the build) and keep the screen up until it's done.
    if (engine().loading()) {
        Scene* sc = &gameObject().scene();
        const uint64_t id = gameObject().id();
        engine().addLoadTask("Finding paths", [sc, id]() -> float {
            GameObject* o = sc->findById(id);
            NavMeshSurface* s = o ? o->getComponent<NavMeshSurface>() : nullptr;
            if (!s) return 1.0f;
            s->pump();
            return s->progress();
        });
    }
}

void NavMeshSurface::build()
{
    m_buildRequested = false;
    destroyNav();
    m_bodies.clear();
    m_foliageShapes.clear();
    m_built = settings;
    NavMeshSettings& s = m_built;
    s.tileSize = std::max(s.tileSize, s.cellSize * 16.0f);
    const Terrain* terrain = Terrain::find(gameObject().scene());
    m_streaming = streamRadius > 0.0f && terrain;

    dtNavMeshParams params{};
    if (m_streaming) {
        const glm::vec2 lo = terrain->boundsMin();
        m_origin = {lo.x, 0.0f, lo.y};
        const float span = 2.0f * streamRadius / s.tileSize + 3.0f;
        params.maxTiles = int(nextPow2(uint32_t(span * span * 1.6f)));
    } else {
        const glm::vec3 c = transform().position();
        m_origin = c - size * 0.5f;
        m_tiles = glm::max(glm::ivec2(glm::ceil(glm::vec2(size.x, size.z) / s.tileSize)), glm::ivec2(1));
        params.maxTiles = int(nextPow2(uint32_t(m_tiles.x * m_tiles.y)));
    }
    dtVcopy(params.orig, ptr(m_origin));
    params.tileWidth = s.tileSize;
    params.tileHeight = s.tileSize;
    params.maxPolys = 1 << 16;
    m_nav = dtAllocNavMesh();
    if (!m_nav || dtStatusFailed(m_nav->init(&params))) {
        log::error("NavMeshSurface: navmesh init failed");
        destroyNav();
        return;
    }
    m_query = dtAllocNavMeshQuery();
    m_query->init(m_nav, 4096);
    m_crowd = dtAllocCrowd();
    m_crowd->init(kMaxAgents, std::max(1.0f, s.agentRadius * 3.0f), m_nav);
    ++m_generation;

    // Tile builds run as pool jobs (queueTile()) on this navmesh's queue.
    m_queue = std::make_shared<BuildQueue>();
    m_queue->settings = m_built;

    if (m_streaming) {
        m_lastFocus = glm::vec2(1e30f);
        updateStreaming();
    } else {
        // Nearest the surface's centre first.
        std::vector<glm::ivec2> order;
        for (int z = 0; z < m_tiles.y; ++z)
            for (int x = 0; x < m_tiles.x; ++x) order.emplace_back(x, z);
        const glm::vec2 mid = glm::vec2(m_tiles) * 0.5f - 0.5f;
        std::sort(order.begin(), order.end(), [&](glm::ivec2 a, glm::ivec2 b) {
            return glm::length(glm::vec2(a) - mid) < glm::length(glm::vec2(b) - mid);
        });
        for (glm::ivec2 t : order) queueTile(t.x, t.y);
    }
}

glm::vec4 NavMeshSurface::tileArea(int tx, int tz) const
{
    const float ts = m_built.tileSize;
    return {m_origin.x + float(tx) * ts, m_origin.z + float(tz) * ts, m_origin.x + float(tx + 1) * ts, m_origin.z + float(tz + 1) * ts};
}

glm::ivec4 NavMeshSurface::tilesTouching(glm::vec4 area) const
{
    const float ts = m_built.tileSize;
    return {int(std::floor((area.x - m_origin.x) / ts)), int(std::floor((area.y - m_origin.z) / ts)),
            int(std::floor((area.z - m_origin.x) / ts)), int(std::floor((area.w - m_origin.z) / ts))};
}

void NavMeshSurface::rebuild(glm::vec4 area)
{
    if (!m_nav) return;
    // The border: geometry this close to a tile changes its edges too.
    const float b = float(borderCells(m_built)) * m_built.cellSize;
    const glm::ivec4 t = tilesTouching(area + glm::vec4(-b, -b, b, b));
    for (int z = t.y; z <= t.w; ++z)
        for (int x = t.x; x <= t.z; ++x)
            if (m_tileVersion.count(tileKey(x, z))) queueTile(x, z);
}

void NavMeshSurface::queueTile(int tx, int tz)
{
    TileJob job;
    job.tx = tx;
    job.tz = tz;
    job.version = m_nextVersion++;
    m_tileVersion[tileKey(tx, tz)] = job.version;
    gather(job);
    {
        std::lock_guard lock(m_queue->mutex);
        m_queue->jobs.push_back(std::move(job));
    }
    m_drains.topUp(buildJobs(), [q = m_queue](const std::atomic<bool>& c) { drain(*q, c); });
}

// Collects the tile's input triangles (main thread: reads the scene).
void NavMeshSurface::gather(TileJob& job)
{
    VE_PROFILE("Navmesh gather");
    const NavMeshSettings& s = m_built;
    const float border = float(borderCells(s)) * s.cellSize;
    const glm::vec4 t = tileArea(job.tx, job.tz);
    const glm::vec4 area(t.x - border, t.y - border, t.z + border, t.w + border);
    float yMin = 1e30f, yMax = -1e30f;
    auto addTri = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, bool blocked) {
        for (const glm::vec3* v : {&a, &b, &c}) {
            job.verts.insert(job.verts.end(), {v->x, v->y, v->z});
            yMin = std::min(yMin, v->y);
            yMax = std::max(yMax, v->y);
        }
        job.blocked.push_back(blocked ? 1 : 0);
    };
    auto overlaps = [&](const glm::vec3& lo, const glm::vec3& hi) {
        return hi.x >= area.x && lo.x <= area.z && hi.z >= area.y && lo.z <= area.w;
    };
    Scene& scene = gameObject().scene();

    const Terrain* terrain = includeTerrain ? Terrain::find(scene) : nullptr;
    if (terrain) {
        const glm::vec2 lo = glm::max(glm::vec2(area.x, area.y), terrain->boundsMin());
        const glm::vec2 hi = glm::min(glm::vec2(area.z, area.w), terrain->boundsMax());
        const float step = std::max(s.terrainStep, s.cellSize);
        const int nx = int(std::ceil((hi.x - lo.x) / step)), nz = int(std::ceil((hi.y - lo.y) / step));
        if (nx > 0 && nz > 0) {
            std::vector<float> h(size_t(nx + 1) * size_t(nz + 1));
            for (int z = 0; z <= nz; ++z)
                for (int x = 0; x <= nx; ++x)
                    h[size_t(z) * size_t(nx + 1) + size_t(x)] =
                        terrain->height(std::min(lo.x + float(x) * step, hi.x), std::min(lo.y + float(z) * step, hi.y));
            const float deep = s.maxWaterDepth >= 0.0f ? terrain->waterLevel - s.maxWaterDepth : -1e30f;
            auto P = [&](int x, int z) {
                return glm::vec3(std::min(lo.x + float(x) * step, hi.x), h[size_t(z) * size_t(nx + 1) + size_t(x)],
                                 std::min(lo.y + float(z) * step, hi.y));
            };
            for (int z = 0; z < nz; ++z) {
                for (int x = 0; x < nx; ++x) {
                    const glm::vec3 a = P(x, z), b = P(x + 1, z), c = P(x, z + 1), d = P(x + 1, z + 1);
                    // Counter-clockwise seen from above (Recast's walkable test uses the winding).
                    addTri(a, c, b, std::max({a.y, b.y, c.y}) < deep);
                    addTri(b, c, d, std::max({b.y, c.y, d.y}) < deep);
                }
            }
        }
    }

    if (includeStaticBodies) {
        ++m_gatherStamp;
        scene.forEach<Rigidbody>([&](Rigidbody& rb) {
            if (rb.type != BodyType::Static || !rb.isActive() || !rb.collider.valid()) return;
            if (dynamic_cast<Terrain*>(&rb.gameObject())) return; // sampled above
            // By address, checked against the object id and the shape: a freed
            // Rigidbody's address can be reused by a new one.
            const glm::mat4& m = rb.transform().worldMatrix();
            BodyCache& cache = m_bodies[&rb];
            cache.seen = m_gatherStamp;
            if (cache.tris.empty() || cache.transform != m || cache.object != rb.gameObject().id() || cache.shape != rb.collider.impl) {
                cache.transform = m;
                cache.object = rb.gameObject().id();
                cache.shape = rb.collider.impl;
                cache.tris = rb.collider.triangles(m);
                cache.lo = glm::vec3(1e30f);
                cache.hi = glm::vec3(-1e30f);
                for (const glm::vec3& v : cache.tris) {
                    cache.lo = glm::min(cache.lo, v);
                    cache.hi = glm::max(cache.hi, v);
                }
            }
            if (!overlaps(cache.lo, cache.hi)) return;
            for (size_t i = 0; i + 2 < cache.tris.size(); i += 3) addTri(cache.tris[i], cache.tris[i + 1], cache.tris[i + 2], false);
        });
        // Bodies gone (or no longer static / active) since the last gather.
        std::erase_if(m_bodies, [&](const auto& entry) { return entry.second.seen != m_gatherStamp; });
    } else {
        m_bodies.clear();
    }

    if (includeFoliage) {
        FoliageSystem* foliage = m_foliage.get(scene);
        if (!foliage) {
            foliage = scene.findFirst<FoliageSystem>();
            m_foliage = foliage;
            m_foliageListener = 0;
        }
        if (foliage) {
            if (!m_foliageListener) {
                // A tree was chopped (or grew back): the paths around it change.
                m_foliageListener = foliage->addStateListener([this](FoliageInstanceId, const glm::vec3& p) {
                    rebuild({p.x - 2.0f, p.z - 2.0f, p.x + 2.0f, p.z + 2.0f});
                });
            }
            // A few shapes shared by thousands of instances: triangulate each
            // once (unplaced), then place the triangles per instance.
            foliage->forEachCollider(area, [&](const Collider& c, const glm::mat4& m) {
                auto it = std::find_if(m_foliageShapes.begin(), m_foliageShapes.end(), [&](const auto& f) { return f.first == c.impl; });
                if (it == m_foliageShapes.end()) it = m_foliageShapes.insert(m_foliageShapes.end(), {c.impl, c.triangles(glm::mat4(1.0f))});
                const std::vector<glm::vec3>& tris = it->second;
                for (size_t i = 0; i + 2 < tris.size(); i += 3)
                    addTri(glm::vec3(m * glm::vec4(tris[i], 1.0f)), glm::vec3(m * glm::vec4(tris[i + 1], 1.0f)),
                           glm::vec3(m * glm::vec4(tris[i + 2], 1.0f)), false);
            });
        }
    }

    job.bmin = {t.x - border, yMin - 1.0f, t.y - border};
    job.bmax = {t.z + border, yMax + 1.0f, t.w + border};
}

// Recast: voxelize the triangles, find the walkable surface, make polygons;
// Detour: pack them as a tile. Worker thread, no scene access.
NavMeshSurface::TileResult NavMeshSurface::buildTile(const NavMeshSettings& s, const TileJob& job)
{
    const auto t0 = std::chrono::steady_clock::now();
    TileResult out{job.tx, job.tz, job.version};
    const int ntris = int(job.blocked.size());
    if (ntris == 0) return out;

    rcContext ctx(false);
    rcConfig cfg{};
    cfg.cs = s.cellSize;
    cfg.ch = s.cellHeight;
    cfg.walkableSlopeAngle = s.maxSlopeDegrees;
    cfg.walkableHeight = int(std::ceil(s.agentHeight / cfg.ch));
    cfg.walkableClimb = int(std::floor(s.agentMaxClimb / cfg.ch));
    cfg.walkableRadius = int(std::ceil(s.agentRadius / cfg.cs));
    cfg.maxEdgeLen = int(12.0f / cfg.cs);
    cfg.maxSimplificationError = 1.3f;
    cfg.minRegionArea = int(s.minRegionArea / (cfg.cs * cfg.cs));
    cfg.mergeRegionArea = int(20.0f / (cfg.cs * cfg.cs));
    cfg.maxVertsPerPoly = 6;
    cfg.tileSize = int(std::round(s.tileSize / cfg.cs));
    cfg.borderSize = borderCells(s);
    cfg.width = cfg.tileSize + cfg.borderSize * 2;
    cfg.height = cfg.tileSize + cfg.borderSize * 2;
    cfg.detailSampleDist = cfg.cs * 6.0f;
    cfg.detailSampleMaxError = cfg.ch * 0.5f;
    rcVcopy(cfg.bmin, ptr(job.bmin));
    rcVcopy(cfg.bmax, ptr(job.bmax));

    std::vector<int> idx(size_t(ntris) * 3);
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = int(i);
    std::vector<unsigned char> areas(size_t(ntris), 0);

    rcHeightfield* solid = rcAllocHeightfield();
    rcCompactHeightfield* chf = nullptr;
    rcContourSet* cset = nullptr;
    rcPolyMesh* pmesh = nullptr;
    rcPolyMeshDetail* dmesh = nullptr;
    auto cleanup = [&] {
        rcFreeHeightField(solid);
        rcFreeCompactHeightfield(chf);
        rcFreeContourSet(cset);
        rcFreePolyMesh(pmesh);
        rcFreePolyMeshDetail(dmesh);
    };
    if (!rcCreateHeightfield(&ctx, *solid, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch)) {
        cleanup();
        return out;
    }
    rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, job.verts.data(), ntris * 3, idx.data(), ntris, areas.data());
    for (int i = 0; i < ntris; ++i)
        if (job.blocked[size_t(i)]) areas[size_t(i)] = RC_NULL_AREA;
    rcRasterizeTriangles(&ctx, job.verts.data(), ntris * 3, idx.data(), areas.data(), ntris, *solid, cfg.walkableClimb);
    rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *solid);
    rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid);
    rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *solid);

    chf = rcAllocCompactHeightfield();
    if (!rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid, *chf) ||
        !rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf) || !rcBuildDistanceField(&ctx, *chf) ||
        !rcBuildRegions(&ctx, *chf, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea)) {
        cleanup();
        return out;
    }
    cset = rcAllocContourSet();
    if (!rcBuildContours(&ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *cset) || cset->nconts == 0) {
        cleanup();
        return out;
    }
    pmesh = rcAllocPolyMesh();
    dmesh = rcAllocPolyMeshDetail();
    if (!rcBuildPolyMesh(&ctx, *cset, cfg.maxVertsPerPoly, *pmesh) ||
        !rcBuildPolyMeshDetail(&ctx, *pmesh, *chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *dmesh) || pmesh->npolys == 0) {
        cleanup();
        return out;
    }
    for (int i = 0; i < pmesh->npolys; ++i) pmesh->flags[i] = pmesh->areas[i] == RC_WALKABLE_AREA ? kWalkFlag : 0;

    dtNavMeshCreateParams p{};
    p.verts = pmesh->verts;
    p.vertCount = pmesh->nverts;
    p.polys = pmesh->polys;
    p.polyAreas = pmesh->areas;
    p.polyFlags = pmesh->flags;
    p.polyCount = pmesh->npolys;
    p.nvp = pmesh->nvp;
    p.detailMeshes = dmesh->meshes;
    p.detailVerts = dmesh->verts;
    p.detailVertsCount = dmesh->nverts;
    p.detailTris = dmesh->tris;
    p.detailTriCount = dmesh->ntris;
    p.walkableHeight = s.agentHeight;
    p.walkableRadius = s.agentRadius;
    p.walkableClimb = s.agentMaxClimb;
    p.tileX = job.tx;
    p.tileY = job.tz;
    p.tileLayer = 0;
    rcVcopy(p.bmin, pmesh->bmin);
    rcVcopy(p.bmax, pmesh->bmax);
    p.cs = cfg.cs;
    p.ch = cfg.ch;
    p.buildBvTree = true;
    unsigned char* data = nullptr;
    int dataSize = 0;
    if (dtCreateNavMeshData(&p, &data, &dataSize)) {
        out.data = data;
        out.size = dataSize;
    }
    cleanup();
    out.ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return out;
}

NavMeshSurface::BuildQueue::~BuildQueue()
{
    for (TileResult& r : results) dtFree(r.data);
}

void NavMeshSurface::drain(BuildQueue& q, const std::atomic<bool>& cancelled)
{
    for (;;) {
        TileJob job;
        {
            std::lock_guard lock(q.mutex);
            if (q.quit || cancelled || q.jobs.empty()) return;
            job = std::move(q.jobs.front());
            q.jobs.pop_front();
            ++q.busy;
        }
        TileResult r = buildTile(q.settings, job);
        {
            std::lock_guard lock(q.mutex);
            --q.busy;
            q.results.push_back(r);
        }
        if (jobs::yieldSuggested()) return; // others are waiting: pump() submits again
    }
}

void NavMeshSurface::applyResults()
{
    std::vector<TileResult> results;
    size_t queued;
    {
        std::lock_guard lock(m_queue->mutex);
        results.swap(m_queue->results);
        queued = m_queue->jobs.size();
        m_stats.pending = uint32_t(queued) + m_queue->busy;
    }
    // Drains that gave their worker back to other jobs: start them again.
    if (queued > 0) {
        m_drains.topUp(std::min(buildJobs(), queued), [q = m_queue](const std::atomic<bool>& c) { drain(*q, c); });
    }
    for (TileResult& r : results) {
        const uint64_t key = tileKey(r.tx, r.tz);
        const auto it = m_tileVersion.find(key);
        if (it == m_tileVersion.end() || it->second != r.version) { // superseded or unloaded
            dtFree(r.data);
            continue;
        }
        if (const dtTileRef old = m_nav->getTileRefAt(r.tx, r.tz, 0)) m_nav->removeTile(old, nullptr, nullptr);
        if (r.data && dtStatusFailed(m_nav->addTile(r.data, r.size, DT_TILE_FREE_DATA, 0, nullptr))) dtFree(r.data);
        m_tilesBuilt.insert(key);
        m_stats.lastTileMs = m_stats.lastTileMs == 0.0f ? r.ms : m_stats.lastTileMs * 0.8f + r.ms * 0.2f;
    }
    if (!results.empty()) {
        m_tilesReady = uint32_t(m_tilesBuilt.size());
        m_stats.tiles = 0;
        m_stats.polygons = 0;
        const dtNavMesh* nav = m_nav;
        for (int i = 0; i < nav->getMaxTiles(); ++i) {
            const dtMeshTile* tile = nav->getTile(i);
            if (!tile || !tile->header) continue;
            ++m_stats.tiles;
            m_stats.polygons += uint32_t(tile->header->polyCount);
        }
    }
}

void NavMeshSurface::updateStreaming()
{
    if (!m_streaming || !m_nav) return;
    glm::vec3 c;
    if (GameObject* f = focus ? gameObject().scene().findById(focus) : nullptr) c = f->transform().position();
    else if (const Camera* cam = gameObject().scene().mainCamera()) c = cam->transform().position();
    else return;
    const glm::vec2 f(c.x, c.z);
    if (glm::length(f - m_lastFocus) < m_built.tileSize * 0.25f) return;
    m_lastFocus = f;

    const float ts = m_built.tileSize;
    const auto tileDist = [&](int tx, int tz) {
        const glm::vec2 centre(m_origin.x + (float(tx) + 0.5f) * ts, m_origin.z + (float(tz) + 0.5f) * ts);
        return glm::length(centre - f);
    };
    // Unload far tiles (with some hysteresis).
    const float keep = streamRadius * 1.25f + ts;
    for (auto it = m_tileVersion.begin(); it != m_tileVersion.end();) {
        const int tx = int(int32_t(uint32_t(it->first >> 32))), tz = int(int32_t(uint32_t(it->first)));
        if (tileDist(tx, tz) > keep) {
            if (const dtTileRef ref = m_nav->getTileRefAt(tx, tz, 0)) m_nav->removeTile(ref, nullptr, nullptr);
            m_tilesBuilt.erase(it->first);
            it = m_tileVersion.erase(it);
        } else {
            ++it;
        }
    }
    m_tilesReady = uint32_t(m_tilesBuilt.size());
    // Load the missing ones, nearest first.
    const glm::ivec4 r = tilesTouching({f.x - streamRadius, f.y - streamRadius, f.x + streamRadius, f.y + streamRadius});
    std::vector<std::pair<float, glm::ivec2>> want;
    for (int z = r.y; z <= r.w; ++z)
        for (int x = r.x; x <= r.z; ++x)
            if (!m_tileVersion.count(tileKey(x, z)) && tileDist(x, z) <= streamRadius + ts * 0.71f) want.push_back({tileDist(x, z), {x, z}});
    std::sort(want.begin(), want.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [d, t] : want) queueTile(t.x, t.y);
}

void NavMeshSurface::pump()
{
    // Once per frame: update() and onGui() (which also runs while the editor pauses) both ask.
    const uint64_t frame = engine().time().frameCount;
    if (frame == m_pumpedFrame) return;
    m_pumpedFrame = frame;
    if (m_buildRequested) build();
    if (!m_nav) return;
    applyResults();
    updateStreaming();
}

float NavMeshSurface::progress() const
{
    if (m_buildRequested) return 0.0f;
    if (!m_nav || m_tileVersion.empty()) return 1.0f;
    uint32_t done = 0;
    for (const auto& [key, version] : m_tileVersion) done += m_tilesBuilt.count(key) ? 1u : 0u;
    // Rebuilds of built tiles don't count as missing.
    return float(done) / float(m_tileVersion.size());
}

void NavMeshSurface::update(float dt)
{
    VE_PROFILE("Navigation");
    pump();
    if (m_crowd && dt > 0.0f) m_crowd->update(dt, nullptr);
}

void NavMeshSurface::onGui()
{
    pump(); // also while the editor pauses the world (a no-op if update() already did this frame)
    // (The "Navmesh" debug channel draws it already: not twice.)
    if (!showDebug || !m_nav || debug::enabled("Navmesh")) return;
    drawDebugLines(renderer().frameData().camera.position, 60.0f);
}

void NavMeshSurface::inspectorGui()
{
    ImGui::Text("Tiles %u (%u pending), polygons %u, %.1f ms per tile", m_stats.tiles, m_stats.pending, m_stats.polygons, m_stats.lastTileMs);
    ImGui::Checkbox("Show navmesh", &showDebug);
    if (ImGui::Button("Rebuild")) build();
}

void NavMeshSurface::drawDebugLines(const glm::vec3& eye, float maxDistance) const
{
    if (!m_nav) return;
    const dtNavMesh* nav = m_nav;
    const glm::vec3 lift(0.0f, 0.08f, 0.0f);
    debug::Style inner, edge;
    inner.thickness = 1.0f;
    edge.thickness = 2.0f;
    for (int i = 0; i < nav->getMaxTiles(); ++i) {
        const dtMeshTile* tile = nav->getTile(i);
        if (!tile || !tile->header) continue;
        const glm::vec3 lo = vec(tile->header->bmin), hi = vec(tile->header->bmax);
        if (glm::distance(glm::clamp(eye, lo, hi), eye) > maxDistance) continue;
        for (int p = 0; p < tile->header->polyCount; ++p) {
            const dtPoly& poly = tile->polys[p];
            if (poly.getType() != DT_POLYTYPE_GROUND) continue;
            if (glm::distance(vec(&tile->verts[poly.verts[0] * 3]), eye) > maxDistance) continue;
            for (int j = 0; j < poly.vertCount; ++j) {
                const glm::vec3 a = vec(&tile->verts[poly.verts[j] * 3]) + lift;
                const glm::vec3 b = vec(&tile->verts[poly.verts[(j + 1) % poly.vertCount] * 3]) + lift;
                const bool wall = poly.neis[j] == 0; // boundary: a wall or the mesh's edge
                debug::line(a, b, wall ? glm::vec4(0.15f, 0.85f, 1.0f, 0.9f) : glm::vec4(0.15f, 0.6f, 1.0f, 0.35f), wall ? edge : inner);
            }
        }
    }
}

void NavAgent::drawDebug() const
{
    const glm::vec3 p = transform().position();
    const glm::vec3 up(0.0f, 0.05f, 0.0f);
    debug::circle(p + up, {0, 1, 0}, radius, onNavMesh() ? debug::kCyan : debug::kRed);
    if (glm::length(m_velocity) > 0.05f) debug::arrow(p + glm::vec3(0, 0.5f, 0), p + glm::vec3(0, 0.5f, 0) + m_velocity * 0.5f, debug::kYellow);
    if (!m_hasTarget) return;
    // The path the crowd steers along: the next corners, then the destination.
    std::vector<glm::vec3> pts{p + up};
    if (const NavMeshSurface* s = surface(); s && s->crowd() && m_index >= 0)
        if (const dtCrowdAgent* ag = s->crowd()->getAgent(m_index); ag && ag->active)
            for (int i = 0; i < ag->ncorners; ++i) pts.push_back(vec(&ag->cornerVerts[i * 3]) + up);
    if (glm::distance(pts.back(), m_target + up) > 0.05f) pts.push_back(m_target + up);
    debug::path(pts, debug::kMagenta);
    debug::cross(m_target + glm::vec3(0, 0.3f, 0), 0.5f, debug::kMagenta);
}

void NavMeshSurface::onDestroy()
{
    if (FoliageSystem* foliage = m_foliageListener ? m_foliage.get(gameObject().scene()) : nullptr) foliage->removeStateListener(m_foliageListener);
    m_foliageListener = 0;
    destroyNav();
}

// --- Queries ------------------------------------------------------------------------

std::optional<glm::vec3> NavMeshSurface::nearestPoint(const glm::vec3& p, const glm::vec3& searchExtents) const
{
    if (!m_query) return std::nullopt;
    dtQueryFilter filter;
    dtPolyRef ref = 0;
    glm::vec3 out;
    if (dtStatusFailed(m_query->findNearestPoly(ptr(p), ptr(searchExtents), &filter, &ref, &out.x)) || !ref) return std::nullopt;
    return out;
}

NavPath NavMeshSurface::findPath(const glm::vec3& from, const glm::vec3& to) const
{
    NavPath out;
    if (!m_query) return out;
    dtQueryFilter filter;
    const glm::vec3 ext(2.0f, 4.0f, 2.0f);
    dtPolyRef startRef = 0, endRef = 0;
    glm::vec3 start, end;
    m_query->findNearestPoly(ptr(from), ptr(ext), &filter, &startRef, &start.x);
    m_query->findNearestPoly(ptr(to), ptr(ext), &filter, &endRef, &end.x);
    if (!startRef) return out;
    if (!endRef) {
        // The goal is off the mesh: head for the closest point we can find.
        const glm::vec3 far(8.0f, 16.0f, 8.0f);
        m_query->findNearestPoly(ptr(to), ptr(far), &filter, &endRef, &end.x);
        if (!endRef) return out;
    }
    dtPolyRef polys[kMaxPathPolys];
    int count = 0;
    const dtStatus st = m_query->findPath(startRef, endRef, ptr(start), ptr(end), &filter, polys, &count, kMaxPathPolys);
    if (dtStatusFailed(st) || count == 0) return out;
    out.complete = polys[count - 1] == endRef && !(st & DT_PARTIAL_RESULT) && glm::distance(end, to) < 0.75f;
    glm::vec3 goal = end;
    if (polys[count - 1] != endRef) m_query->closestPointOnPoly(polys[count - 1], ptr(end), &goal.x, nullptr);
    float straight[kMaxPathPolys * 3];
    int ncorners = 0;
    m_query->findStraightPath(ptr(start), ptr(goal), polys, count, straight, nullptr, nullptr, &ncorners, kMaxPathPolys);
    for (int i = 0; i < ncorners; ++i) out.corners.push_back(vec(&straight[i * 3]));
    return out;
}

std::optional<glm::vec3> NavMeshSurface::randomPointNear(const glm::vec3& center, float radius)
{
    if (!m_query) return std::nullopt;
    dtQueryFilter filter;
    dtPolyRef startRef = 0;
    glm::vec3 start;
    const glm::vec3 ext(2.0f, 4.0f, 2.0f);
    m_query->findNearestPoly(ptr(center), ptr(ext), &filter, &startRef, &start.x);
    if (!startRef) return std::nullopt;
    t_rng = &m_rng;
    for (int attempt = 0; attempt < 8; ++attempt) {
        dtPolyRef ref = 0;
        glm::vec3 p;
        if (dtStatusFailed(m_query->findRandomPointAroundCircle(startRef, ptr(start), radius, &filter, frand, &ref, &p.x)) || !ref) break;
        // Detour picks whole polygons touching the circle: keep points inside it.
        if (glm::length(glm::vec2(p.x - center.x, p.z - center.z)) <= radius) return p;
    }
    return start;
}

std::optional<glm::vec3> NavMeshSurface::raycast(const glm::vec3& from, const glm::vec3& to) const
{
    if (!m_query) return std::nullopt;
    dtQueryFilter filter;
    dtPolyRef startRef = 0;
    glm::vec3 start;
    const glm::vec3 ext(2.0f, 4.0f, 2.0f);
    m_query->findNearestPoly(ptr(from), ptr(ext), &filter, &startRef, &start.x);
    if (!startRef) return from;
    float t = 0.0f;
    glm::vec3 normal;
    dtPolyRef polys[kMaxPathPolys];
    int count = 0;
    m_query->raycast(startRef, ptr(start), ptr(to), &filter, &t, &normal.x, polys, &count, kMaxPathPolys);
    if (t > 1.0f) return std::nullopt; // FLT_MAX: reached `to`
    return start + (to - start) * t;
}

// =============================================================================
// NavAgent
// =============================================================================

NavMeshSurface* NavAgent::surface() const
{
    GameObject* o = m_surface ? gameObject().scene().findById(m_surface) : nullptr;
    return o ? o->getComponent<NavMeshSurface>() : nullptr;
}

void NavAgent::start()
{
    const glm::vec3 f = transform().forward();
    m_yaw = std::atan2(-f.x, -f.z);
    m_yawInit = true;
}

void NavAgent::applyParams(NavMeshSurface* s, bool force)
{
    if (!s || !s->crowd() || m_index < 0) return;
    // Only when a setting changed: lateUpdate asks every frame, and they rarely do.
    const PushedParams now{radius, height, acceleration, speed, avoidOthers};
    if (!force && now == m_pushed) return;
    m_pushed = now;
    dtCrowdAgentParams p{};
    p.radius = radius;
    p.height = height;
    p.maxAcceleration = acceleration;
    p.maxSpeed = speed;
    p.collisionQueryRange = radius * 12.0f;
    p.pathOptimizationRange = radius * 30.0f;
    p.updateFlags = DT_CROWD_ANTICIPATE_TURNS | DT_CROWD_OPTIMIZE_VIS | DT_CROWD_OPTIMIZE_TOPO;
    if (avoidOthers) p.updateFlags |= DT_CROWD_OBSTACLE_AVOIDANCE | DT_CROWD_SEPARATION;
    p.separationWeight = 2.0f;
    p.obstacleAvoidanceType = 3;
    p.queryFilterType = 0;
    s->crowd()->updateAgentParameters(m_index, &p);
}

bool NavAgent::attach()
{
    NavMeshSurface* s = m_surface ? surface() : nullptr;
    if (!s) {
        s = NavMeshSurface::find(gameObject().scene());
        m_surface = s ? s->gameObject().id() : 0;
    }
    if (!s || !s->crowd() || !s->ready()) return false;
    dtCrowdAgentParams p{};
    p.radius = radius;
    p.height = height;
    p.maxAcceleration = acceleration;
    p.maxSpeed = speed;
    const glm::vec3 pos = transform().position();
    // Only join once there's ground under us (tiles build over a few frames).
    if (!s->nearestPoint(pos)) return false;
    m_index = s->crowd()->addAgent(ptr(pos), &p);
    if (m_index < 0) return false;
    m_generation = s->generation();
    applyParams(s, true);
    if (m_pendingTarget || m_hasTarget) {
        m_pendingTarget = false;
        setDestination(m_target);
    }
    return true;
}

void NavAgent::detach()
{
    if (m_index >= 0) {
        NavMeshSurface* s = surface();
        if (s && s->crowd() && s->generation() == m_generation) s->crowd()->removeAgent(m_index);
    }
    m_index = -1;
}

bool NavAgent::setDestination(const glm::vec3& target)
{
    m_target = target;
    m_hasTarget = true;
    NavMeshSurface* s = surface();
    if (m_index < 0 || !s || !s->crowd()) {
        m_pendingTarget = true;
        return false;
    }
    dtCrowd* crowd = s->crowd();
    const dtNavMeshQuery* q = crowd->getNavMeshQuery();
    dtPolyRef ref = 0;
    glm::vec3 p;
    const glm::vec3 ext(4.0f, 8.0f, 4.0f);
    q->findNearestPoly(ptr(target), ptr(ext), crowd->getFilter(0), &ref, &p.x);
    if (!ref) return false;
    m_target = p;
    return crowd->requestMoveTarget(m_index, ref, ptr(p));
}

void NavAgent::stop()
{
    m_hasTarget = false;
    m_pendingTarget = false;
    NavMeshSurface* s = surface();
    if (m_index >= 0 && s && s->crowd()) s->crowd()->resetMoveTarget(m_index);
}

void NavAgent::warp(const glm::vec3& position)
{
    transform().setPosition(position);
    if (m_index >= 0) {
        detach();
        attach();
    }
}

bool NavAgent::arrived() const
{
    return arrivedOn(m_hasTarget ? surface() : nullptr);
}

bool NavAgent::arrivedOn(const NavMeshSurface* s) const
{
    if (!m_hasTarget) return true;
    if (m_index < 0 || !s || !s->crowd()) return false;
    const dtCrowdAgent* ag = s->crowd()->getAgent(m_index);
    if (!ag || !ag->active || ag->targetState != DT_CROWDAGENT_TARGET_VALID) return false;
    const glm::vec3 pos = vec(ag->npos);
    if (glm::distance(pos, m_target) <= stoppingDistance) return true;
    // The goal may be unreachable: the path then ends at the closest point.
    if (ag->ncorners == 0) return true;
    const int last = ag->ncorners - 1;
    return (ag->cornerFlags[last] & DT_STRAIGHTPATH_END) && glm::distance(pos, vec(&ag->cornerVerts[last * 3])) <= stoppingDistance;
}

float NavAgent::remainingDistance() const
{
    if (!m_hasTarget) return 0.0f;
    NavMeshSurface* s = surface();
    if (m_index < 0 || !s || !s->crowd()) return glm::distance(transform().position(), m_target);
    const dtCrowdAgent* ag = s->crowd()->getAgent(m_index);
    glm::vec3 p = vec(ag->npos);
    float d = 0.0f;
    for (int i = 0; i < ag->ncorners; ++i) {
        const glm::vec3 c = vec(&ag->cornerVerts[i * 3]);
        d += glm::distance(p, c);
        p = c;
    }
    if (ag->ncorners == 0 || !(ag->cornerFlags[ag->ncorners - 1] & DT_STRAIGHTPATH_END)) d += glm::distance(p, m_target);
    return d;
}

void NavAgent::lateUpdate(float dt)
{
    NavMeshSurface* s = surface();
    if (m_index >= 0 && (!s || !s->crowd() || s->generation() != m_generation)) m_index = -1; // the navmesh was rebuilt
    if (m_index < 0) {
        if (!attach()) {
            m_velocity = glm::vec3(0.0f);
            return;
        }
        s = surface(); // (attach may have found it)
    }
    applyParams(s, false);
    const dtCrowdAgent* ag = s->crowd()->getAgent(m_index);
    if (!ag || !ag->active) return;
    if (m_hasTarget && arrivedOn(s)) {
        // Stop exactly: the crowd would otherwise creep the last bit.
        s->crowd()->resetMoveTarget(m_index);
        m_hasTarget = false;
    }
    m_velocity = vec(ag->vel);
    transform().setPosition(vec(ag->npos));
    if (updateRotation) {
        const glm::vec2 v(m_velocity.x, m_velocity.z);
        if (glm::length(v) > 0.1f) {
            if (!m_yawInit) start();
            const float want = std::atan2(-v.x, -v.y);
            const float d = std::remainder(want - m_yaw, 6.2831853f);
            const float maxTurn = glm::radians(angularSpeed) * dt;
            m_yaw += std::clamp(d, -maxTurn, maxTurn);
            transform().setRotation(glm::angleAxis(m_yaw, glm::vec3(0, 1, 0)));
        }
    }
}

void NavAgent::onDestroy()
{
    detach();
}

} // namespace ve

// -----------------------------------------------------------------------------
// Reflection
// -----------------------------------------------------------------------------

#include "ve/scene/Reflection.hpp"

namespace ve {

void registerNavigationTypes(TypeRegistry& r)
{
    r.add<NavMeshSurface>("NavMeshSurface", [](TypeBuilder<NavMeshSurface>& t) {
        t.category("Navigation");
        t.field("size", &NavMeshSurface::size).tooltip("box centred on the object that agents can use");
        auto setting = [&t](const char* name, float NavMeshSettings::*m, float lo, float hi, const char* tip) {
            t.property(name, FieldKind::Float, [m](const NavMeshSurface& s) { return Json(s.settings.*m); },
                       [m, lo, hi](NavMeshSurface& s, const Json& j) {
                           if (j.is_number()) s.settings.*m = std::clamp(j.get<float>(), lo, hi);
                       })
                .tooltip(tip);
        };
        setting("agentRadius", &NavMeshSettings::agentRadius, 0.05f, 5.0f, "the navmesh keeps this far from walls");
        setting("agentHeight", &NavMeshSettings::agentHeight, 0.1f, 10.0f, "headroom needed");
        setting("agentMaxClimb", &NavMeshSettings::agentMaxClimb, 0.0f, 5.0f, "highest step");
        setting("maxSlopeDegrees", &NavMeshSettings::maxSlopeDegrees, 0.0f, 89.0f, "steeper ground is not walkable");
        setting("cellSize", &NavMeshSettings::cellSize, 0.05f, 2.0f, "voxel size (about agentRadius / 2)");
        setting("tileSize", &NavMeshSettings::tileSize, 4.0f, 256.0f, "m; changes rebuild whole tiles");
        setting("maxWaterDepth", &NavMeshSettings::maxWaterDepth, -1.0f, 100.0f, "deeper water is not walkable (< 0: no limit)");
        t.field("includeTerrain", &NavMeshSurface::includeTerrain);
        t.field("includeStaticBodies", &NavMeshSurface::includeStaticBodies);
        t.field("includeFoliage", &NavMeshSurface::includeFoliage);
        t.field("streamRadius", &NavMeshSurface::streamRadius).range(0.0f, 4000.0f).tooltip("> 0: only tiles this close to the camera exist (whole terrain)");
        t.field("showDebug", &NavMeshSurface::showDebug);
    });
    r.add<NavAgent>("NavAgent", [](TypeBuilder<NavAgent>& t) {
        t.category("Navigation");
        t.field("speed", &NavAgent::speed).range(0.0f, 50.0f).speed(0.01f);
        t.field("acceleration", &NavAgent::acceleration).range(0.0f, 200.0f);
        t.field("angularSpeed", &NavAgent::angularSpeed).range(0.0f, 2000.0f);
        t.field("radius", &NavAgent::radius).range(0.05f, 5.0f).speed(0.01f);
        t.field("height", &NavAgent::height).range(0.1f, 10.0f);
        t.field("stoppingDistance", &NavAgent::stoppingDistance).range(0.0f, 20.0f);
        t.field("updateRotation", &NavAgent::updateRotation);
        t.field("avoidOthers", &NavAgent::avoidOthers);
    });
}

} // namespace ve
