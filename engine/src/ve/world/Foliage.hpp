#pragma once

// =============================================================================
// FoliageSystem - streamed, rule-based scattering of instanced props (trees,
// bushes, rocks...) over a world of any size.
//
//   ve::FoliageWorld world;
//   world.boundsMin = {-2000, -2000}; world.boundsMax = {2000, 2000};
//   world.height = [&](glm::vec2 p) { return terrain.height(p.x, p.y); };   // thread-safe!
//   auto& foliage = scene.spawn<ve::FoliageSystem>(world);
//
//   ve::FoliageLayer trees;
//   trees.variants = {acaciaLods0, acaciaLods1};       // one LOD chain per mesh variant
//   trees.colliders = {ve::Collider::capsule(2.0f, 0.3f)};
//   trees.spacing = 9.0f;                              // at most one tree per 9x9 m
//   trees.density = [&](const ve::FoliageSample& s) { return biome.savannah(s.position); };
//   trees.viewDistance = 1500.0f;
//   const uint32_t treeLayer = foliage.addLayer(trees);
//
//   ve::FoliageLayer bushes = ...;
//   bushes.avoid = {{treeLayer, 3.0f}};                // keep 3 m from any tree
//   foliage.addLayer(bushes);
//   foliage.preload(spawnPosition);                    // no pop-in at the start
//
// Placement is a pure function of (layer seed, grid cell): each layer puts at
// most one candidate in every `spacing` square (jittered grid) and keeps it if
// it passes the rules (bounds, exclusion circles, probability, noise
// clustering, height/slope range, the density callback, distance to other
// layers' instances). So the same world appears whatever order cells load in,
// and nothing needs to be stored or saved.
//
// Streaming: the world is split into square cells (FoliageSettings::cellSize).
// A worker thread generates (cell, layer) pairs nearest-first around the camera
// out to each layer's viewDistance. The main thread adds the results to the
// layer's InstanceGroups under a per-frame budget, and removes cells that fall
// behind. Static colliders are created only for cells within
// colliderDistance of `colliderFocus` (the player), also budgeted.
//
// Thread safety: the FoliageWorld and layer callbacks run on the worker
// thread. They must only read data that doesn't change while the system is
// alive. Add layers and exclusions before the first update/preload.
// =============================================================================

#include "ve/physics/Physics.hpp"
#include "ve/render/Instancing.hpp"
#include "ve/scene/GameObject.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cfloat>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ve {

// What the generator needs to know about the ground.
struct FoliageWorld {
    glm::vec2 boundsMin{-1e6f}, boundsMax{1e6f};
    std::function<float(glm::vec2)> height;     // required
    std::function<glm::vec3(glm::vec2)> normal; // optional: default = from height differences
};

// A candidate position handed to FoliageLayer::density.
struct FoliageSample {
    glm::vec2 position; // world x/z
    float height;
    glm::vec3 normal;
};

// Placement and streaming rules: shared by FoliageLayer (code) and the
// FoliageType scene component (editor / scene files), which derive from it.
struct FoliageRules {
    // ---- Where (all optional except spacing) ----
    float spacing = 4.0f;    // one candidate per spacing x spacing square
    float jitter = 0.8f;     // 0 = perfect grid, 1 = anywhere in the square (can touch neighbours)
    float probability = 1.0f;
    float minHeight = -FLT_MAX, maxHeight = FLT_MAX;
    float maxSlope = 1.0f;   // 1 - normal.y (0 = flat only, 1 = any)
    // Noise clustering (groves, patches): 0 = off. Frequency in 1/m;
    // coverage = rough fraction of the world the clusters cover.
    float clusterFrequency = 0.0f;
    float clusterCoverage = 0.5f;

    // ---- How ----
    float minScale = 1.0f, maxScale = 1.0f;
    float tint = 0.1f;          // +-brightness variation (InstanceGroup custom.rgb, used by standard.vert)
    float alignToNormal = 0.0f; // 0 = upright, 1 = follow the slope
    float sink = 0.0f;          // push down into the ground (m at scale 1)

    // ---- Streaming / rendering ----
    float viewDistance = 500.0f;     // generated, loaded and drawn within this distance of the camera
    float shadowDistance = FLT_MAX;
    bool castShadows = true;
    float colliderDistance = 60.0f;  // colliders exist within this distance of colliderFocus
    uint32_t maxInstances = 0;       // loaded at once; 0 = estimate from viewDistance/spacing
    uint32_t seed = 0;
};

struct FoliageLayer : FoliageRules {
    std::string name = "foliage";

    // ---- What ----
    std::vector<std::vector<InstanceLod>> variants; // one LOD chain per mesh variant (picked at random)
    std::vector<Collider> colliders;                // none / one shared / one per variant
    uint32_t colliderUserValue = 0;                 // RaycastHit::userValue of this layer's colliders

    // ---- More "where" ----
    // Biome rules: return the chance (0..1) of keeping a candidate.
    std::function<float(const FoliageSample&)> density;
    // Keep at least `radius` metres from instances of an earlier layer.
    struct Avoid {
        uint32_t layer;
        float radius;
    };
    std::vector<Avoid> avoid;
};

struct FoliageSettings {
    float cellSize = 64.0f;
    uint32_t instancesPerFrame = 6000; // budget for adding instances to groups
    uint32_t bodiesPerFrame = 300;     // budget for creating static bodies
    float unloadMargin = 1.15f;        // unload beyond viewDistance * margin (hysteresis)
};

struct FoliageStats {
    uint32_t cells = 0;      // (cell, layer) pairs loaded or loading
    uint32_t pending = 0;    // waiting for the worker
    uint32_t instances = 0;
    uint32_t bodies = 0;
    uint32_t dropped = 0;    // instances skipped because a layer's maxInstances was reached
    float generateMs = 0.0f; // worker time per (cell, layer), smoothed
};

class FoliageSystem : public GameObject {
public:
    explicit FoliageSystem(FoliageWorld world, FoliageSettings settings = {});
    ~FoliageSystem() override;

    uint32_t addLayer(FoliageLayer layer);
    void addExclusion(glm::vec2 center, float radius); // nothing is placed inside (camps, roads...)

    // Synchronously loads everything visible from `viewer` (+ colliders).
    void preload(const glm::vec3& viewer);

    // Colliders are streamed around this object (e.g. the player); null = the camera.
    GameObject* colliderFocus = nullptr;

    void update(float dt) override;
    void onGui() override;
    const FoliageStats& stats() const { return m_stats; }

    // Instances layer `layer` would place near `p` (deterministic, works for unloaded cells too).
    std::vector<glm::vec3> query(uint32_t layer, glm::vec2 p, float radius) const;

private:
    struct Instance {
        uint32_t variant;
        glm::vec3 position;
        glm::quat rotation;
        float scale;
        glm::vec4 custom;
    };
    struct Candidate {
        glm::vec2 p;
        float h;
        glm::vec3 n;
        uint32_t variant;
        float scale, yaw, tint;
    };
    struct LayerData {
        FoliageLayer desc;
        std::vector<std::shared_ptr<InstanceGroup>> groups; // per variant
        uint32_t capacityWarned = 0;
    };
    enum class CellState { Queued, Ready, Loaded };
    struct Cell {
        uint32_t layer;
        int cx, cz;
        uint32_t ticket;
        CellState state = CellState::Queued;
        std::vector<Instance> instances;
        size_t applied = 0; // instances added so far
        std::vector<std::pair<uint32_t, InstanceGroup::Handle>> handles;
        std::vector<uint32_t> bodies;
        size_t bodiesCreated = 0;
        bool wantBodies = false;
    };
    struct Job {
        uint64_t key;
        uint32_t ticket;
        uint32_t layer;
        int cx, cz;
    };
    struct Result {
        uint64_t key;
        uint32_t ticket;
        std::vector<Instance> instances;
        float ms;
    };

    static uint64_t cellKey(uint32_t layer, int cx, int cz);
    bool candidate(uint32_t layer, int gx, int gz, Candidate* out) const;
    std::vector<Instance> generate(uint32_t layer, int cx, int cz) const;
    float cellDistance(int cx, int cz, glm::vec2 p) const;
    bool cellInBounds(int cx, int cz) const;
    glm::ivec4 cellRange(int x0, int z0, int x1, int z1) const;
    void ensureGroups(LayerData& layer);
    void startWorker();
    void workerLoop();
    size_t applyInstances(Cell& cell, size_t budget);
    void unload(Cell& cell);
    void destroyBodies(Cell& cell);
    glm::vec3 viewerPosition() const;
    glm::vec3 focusPosition(const glm::vec3& viewer) const;

    FoliageWorld m_world;
    FoliageSettings m_settings;
    std::vector<LayerData> m_layers;
    std::vector<std::pair<glm::vec2, float>> m_exclusions;
    std::unordered_map<uint64_t, Cell> m_cells;
    uint32_t m_nextTicket = 1;
    FoliageStats m_stats;

    std::thread m_worker;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::vector<Job> m_jobs;
    std::vector<Result> m_results;
    glm::vec2 m_workerViewer{0.0f};
    bool m_quit = false;
};

} // namespace ve
