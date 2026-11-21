#pragma once

// =============================================================================
// Navigation - navmeshes, path finding and walking agents (Recast & Detour).
//
// The engine builds where things can walk and moves agents along paths around
// each other; the game decides where they want to go (AI behaviours).
//
//   // Once per level: a surface over the area agents use (here 240 x 240 m).
//   auto& nav = scene.spawn("Navigation").addComponent<ve::NavMeshSurface>();
//   nav.size = {240.0f, 200.0f, 240.0f};          // centred on the object
//   nav.settings.agentRadius = 0.4f;
//
//   // A walker: speed, avoidance of other agents, arrival.
//   auto& agent = villager.addComponent<ve::NavAgent>();
//   agent.speed = 1.3f;
//   agent.setDestination(spot);
//   if (agent.arrived()) ...
//
//   // Queries (AI decisions, spawning):
//   if (auto* nav = ve::NavMeshSurface::find(scene)) {
//       ve::NavPath path = nav->findPath(a, b);       // corner points
//       auto p = nav->randomPointNear(home, 10.0f);   // somewhere reachable
//   }
//
// Geometry: the terrain (heights sampled directly, deep water left out),
// static Rigidbodies and foliage colliders (trees, rocks; also in unloaded
// cells). The navmesh is tiled: tiles build on the job pool (a loading
// task while a loading screen shows), and when a foliage instance changes
// state (a tree is chopped) the tiles around it rebuild. Call rebuild(area)
// after other changes (moved buildings, terrain edits). With streamRadius > 0
// only the tiles around the camera (or `focus`) exist, for big worlds.
// =============================================================================

#include "ve/core/Jobs.hpp"
#include "ve/scene/Components.hpp"
#include "ve/scene/ObjectRef.hpp"

#include <glm/glm.hpp>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class dtNavMesh;
class dtNavMeshQuery;
class dtCrowd;

namespace ve {

class FoliageSystem;
class Rigidbody;

struct NavMeshSettings {
    float cellSize = 0.25f;          // voxel size across (m): about agentRadius / 2
    float cellHeight = 0.2f;         // voxel size up (m)
    float agentRadius = 0.4f;        // the navmesh keeps this far from walls
    float agentHeight = 1.8f;        // needs this much headroom
    float agentMaxClimb = 0.5f;      // steps up to this
    float maxSlopeDegrees = 40.0f;   // steeper ground is not walkable
    float tileSize = 32.0f;          // m; a change rebuilds only the tiles it touches
    float minRegionArea = 6.0f;      // m²: smaller islands (tops of rocks) are dropped
    float terrainStep = 1.0f;        // m between terrain height samples
    float maxWaterDepth = 0.3f;      // terrain deeper under Terrain::waterLevel is not walkable (< 0: no limit)
};

// A path as straight segments between corners (from the start to the end).
struct NavPath {
    std::vector<glm::vec3> corners;
    bool complete = false; // false: the goal was unreachable, the path ends as close as it gets
    float length() const;
};

class NavMeshSurface : public Component {
public:
    NavMeshSurface();
    ~NavMeshSurface() override;

    // --- Settings (read by build()) --------------------------------------------
    NavMeshSettings settings;
    glm::vec3 size{256.0f, 400.0f, 256.0f}; // box centred on the object (ignored for streaming: the whole terrain)
    bool includeTerrain = true;
    bool includeStaticBodies = true;
    bool includeFoliage = true;
    float streamRadius = 0.0f;  // > 0: only tiles within this distance of `focus` exist
    uint64_t focus = 0;          // streaming centre: an object id (e.g. the player's); 0 = the main camera
    bool buildOnStart = true;
    bool showDebug = false;      // draws the navmesh's polygons near the camera

    // --- Building ----------------------------------------------------------------
    void build();                      // (re)creates everything from the current settings
    void rebuild(glm::vec4 area);      // the tiles touching world x0, z0, x1, z1 (geometry changed)
    float progress() const;            // built / wanted tiles (1 when idle)
    bool ready() const { return m_nav != nullptr && m_tilesReady > 0; }

    // --- Queries (main thread) -------------------------------------------------------
    std::optional<glm::vec3> nearestPoint(const glm::vec3& p, const glm::vec3& searchExtents = {2.0f, 4.0f, 2.0f}) const;
    NavPath findPath(const glm::vec3& from, const glm::vec3& to) const;
    // A random reachable point within about `radius` of `center` (connected to it).
    std::optional<glm::vec3> randomPointNear(const glm::vec3& center, float radius);
    // Walks the navmesh from `from` towards `to`: the point where an edge blocks the way, or nothing if clear.
    std::optional<glm::vec3> raycast(const glm::vec3& from, const glm::vec3& to) const;

    struct Stats {
        uint32_t tiles = 0, polygons = 0, pending = 0;
        float lastTileMs = 0.0f; // worker time to build one tile, smoothed
    };
    const Stats& stats() const { return m_stats; }

    static NavMeshSurface* find(const Scene& scene); // the first one

    void start() override;
    void update(float dt) override;
    void onGui() override;
    void onDestroy() override;
    void inspectorGui();   // stats + rebuild button (F1 overlay, editor)
    void drawDebugLines(const glm::vec3& eye, float maxDistance) const; // ve::debug ("Navmesh" channel, showDebug)

    // Engine internal (NavAgent).
    dtCrowd* crowd() const { return m_crowd; }
    uint32_t generation() const { return m_generation; }

private:
    struct TileJob {
        int tx, tz;
        uint32_t version;
        std::vector<float> verts;      // xyz per vertex, 3 vertices per triangle
        std::vector<uint8_t> blocked;  // per triangle: 1 = never walkable (deep water)
        glm::vec3 bmin, bmax;          // the tile's bounds (+ border)
    };
    struct TileResult {
        int tx, tz;
        uint32_t version;
        unsigned char* data = nullptr; // dtNavMesh tile data (owned until added)
        int size = 0;
        float ms = 0.0f;
    };
    struct BodyCache {
        glm::mat4 transform;
        uint64_t object = 0;                // the body's object id and shape (Collider::impl) when cached
        std::shared_ptr<const void> shape;
        uint32_t seen = 0;                  // m_gatherStamp of the last gather that met it
        std::vector<glm::vec3> tris;
        glm::vec3 lo, hi;
    };

    static uint64_t tileKey(int tx, int tz) { return (uint64_t(uint32_t(tx)) << 32) | uint32_t(tz); }
    void destroyNav();
    void pump(); // applies finished tiles, streams; also drives the build behind a loading screen
    void queueTile(int tx, int tz);
    void gather(TileJob& job);
    static TileResult buildTile(const NavMeshSettings& s, const TileJob& job);
    // What the build jobs share with the main thread, for one dtNavMesh's
    // lifetime. Owned jointly: destroyNav() drops it without waiting for a
    // tile in flight (whose result then goes nowhere; ~BuildQueue frees it).
    struct BuildQueue {
        NavMeshSettings settings; // m_built when the navmesh was created
        std::mutex mutex;
        std::deque<TileJob> jobs;
        std::vector<TileResult> results;
        uint32_t busy = 0;
        bool quit = false;
        ~BuildQueue();
    };
    static void drain(BuildQueue& q, const std::atomic<bool>& cancelled); // a pool job
    void applyResults();
    void updateStreaming();
    glm::vec4 tileArea(int tx, int tz) const; // world x0, z0, x1, z1
    glm::ivec4 tilesTouching(glm::vec4 area) const;

    dtNavMesh* m_nav = nullptr;
    dtNavMeshQuery* m_query = nullptr;
    dtCrowd* m_crowd = nullptr;
    NavMeshSettings m_built;      // the settings the current navmesh uses
    glm::vec3 m_origin{0.0f};     // world position of tile (0, 0)'s corner
    glm::ivec2 m_tiles{0};        // tile counts (bounded mode)
    bool m_streaming = false;
    glm::vec2 m_lastFocus{1e30f};
    uint32_t m_generation = 0;    // bumped when the dtNavMesh is recreated
    uint32_t m_tilesReady = 0;
    std::unordered_map<uint64_t, uint32_t> m_tileVersion; // wanted tiles -> latest job version
    std::unordered_set<uint64_t> m_tilesBuilt;
    uint32_t m_nextVersion = 1;
    std::unordered_map<const Rigidbody*, BodyCache> m_bodies; // static Rigidbody -> world triangles
    uint32_t m_gatherStamp = 0;
    // Foliage collider shape -> its triangles, unplaced (kept alive, so the key can't be reused).
    std::vector<std::pair<std::shared_ptr<const void>, std::vector<glm::vec3>>> m_foliageShapes;
    uint64_t m_pumpedFrame = ~0ull;
    ObjectRef<FoliageSystem> m_foliage; // whose colliders and chop events feed the navmesh
    uint32_t m_foliageListener = 0;
    bool m_buildRequested = false;
    std::mt19937 m_rng{1234u};
    Stats m_stats;

    std::shared_ptr<BuildQueue> m_queue; // null without a navmesh
    jobs::DrainGroup m_drains;
};

class NavAgent : public Component {
public:
    float speed = 3.5f;            // m/s
    float acceleration = 8.0f;     // m/s²
    float angularSpeed = 360.0f;   // degrees/s turning towards where it walks
    float radius = 0.4f;           // for avoiding other agents
    float height = 1.8f;
    float stoppingDistance = 0.3f; // arrived within this
    bool updateRotation = true;    // face the walking direction (forward = -Z)
    bool avoidOthers = true;

    // Walks to the closest reachable point to `target`. False if not on a navmesh (yet).
    bool setDestination(const glm::vec3& target);
    void stop();                          // stands still where it is (keeps avoiding others)
    void warp(const glm::vec3& position); // teleports (use instead of moving the Transform)

    bool hasDestination() const { return m_hasTarget; }
    bool arrived() const;                 // no destination, or within stoppingDistance of it
    bool onNavMesh() const { return m_index >= 0; }
    glm::vec3 destination() const { return m_target; }
    glm::vec3 velocity() const { return m_velocity; }
    float remainingDistance() const;      // along the path corners
    void drawDebug() const;               // path corners, destination, velocity (ve::debug, "Nav agents")

    void start() override;
    void lateUpdate(float dt) override;
    void onDestroy() override;

private:
    NavMeshSurface* surface() const;
    bool attach();
    void detach();
    void applyParams(NavMeshSurface* s, bool force); // force: push even if nothing changed
    bool arrivedOn(const NavMeshSurface* s) const;

    struct PushedParams {
        float radius, height, acceleration, speed;
        bool avoidOthers;
        bool operator==(const PushedParams&) const = default;
    };

    uint64_t m_surface = 0;       // NavMeshSurface's object id
    uint32_t m_generation = 0;
    int m_index = -1;             // dtCrowd agent index
    glm::vec3 m_target{0.0f};
    glm::vec3 m_velocity{0.0f};
    bool m_hasTarget = false;
    bool m_pendingTarget = false; // set before the agent was on the navmesh
    float m_yaw = 0.0f;
    bool m_yawInit = false;
    PushedParams m_pushed{};      // what applyParams last gave the crowd
};

class TypeRegistry;
// Reflection (scene files, editor): this module's components. The Engine calls it at startup.
void registerNavigationTypes(TypeRegistry& r);

} // namespace ve
