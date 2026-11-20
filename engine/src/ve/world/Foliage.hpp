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
// Jobs on the engine's worker pool (ve/core/Jobs.hpp) generate (cell, layer)
// pairs nearest-first around the camera out to each layer's viewDistance. The main thread adds the results to the
// layer's InstanceGroups under a per-frame budget, and removes cells that fall
// behind. Static colliders are created only for cells within
// colliderDistance of the collider focus (the player), also budgeted.
//
// Thread safety: the FoliageWorld and layer callbacks run on worker threads
// (several at once). They must only read data that doesn't change while the
// system is alive. Add layers and exclusions before the first update/preload.
// =============================================================================

#include "ve/core/Jobs.hpp"
#include "ve/physics/Physics.hpp"
#include "ve/render/Instancing.hpp"
#include "ve/scene/GameObject.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cfloat>
#include <functional>
#include <memory>
#include <optional>
#include <mutex>
#include <string>
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

// Stable identity of one placed instance: its layer and placement grid
// square. Placement is deterministic, so the id is the same every run and can
// be stored (save games, quest state).
struct FoliageInstanceId {
    uint32_t layer = 0xFFFFFFFFu;
    int gx = 0, gz = 0;
    bool valid() const { return layer != 0xFFFFFFFFu; }
    uint64_t packed() const
    {
        return (uint64_t(layer & 0xFFFFu) << 48) | (uint64_t(uint32_t(gx) & 0xFFFFFFu) << 24) | uint64_t(uint32_t(gz) & 0xFFFFFFu);
    }
    static FoliageInstanceId unpack(uint64_t k)
    {
        auto s24 = [](uint64_t v) { return int(int32_t(uint32_t(v & 0xFFFFFFu) << 8) >> 8); };
        return {uint32_t(k >> 48), s24(k >> 24), s24(k)};
    }
    bool operator==(const FoliageInstanceId&) const = default;
};

// State value meaning "not there" (felled and cleared, harvested away...).
constexpr uint32_t kFoliageRemoved = 0xFFFFFFFFu;

struct FoliageInstanceInfo {
    FoliageInstanceId id;
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    float scale = 1.0f;
    uint32_t variant = 0;
    uint32_t state = 0; // 0 = the layer's own look, n = FoliageLayer::states[n - 1], or kFoliageRemoved
};

// An alternative look an instance can be switched to (a stump, a picked bush).
struct FoliageState {
    std::string name;
    std::vector<std::vector<InstanceLod>> variants; // one per layer variant, or a single shared one; empty = invisible
    std::vector<Collider> colliders;                // none / one shared / one per variant
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
    float colliderDistance = 60.0f;  // colliders exist within this distance of the collider focus
    uint32_t maxInstances = 0;       // loaded at once; 0 = estimate from viewDistance/spacing
    uint32_t seed = 0;
};

struct FoliageLayer : FoliageRules {
    std::string name = "foliage";

    // ---- What ----
    std::vector<std::vector<InstanceLod>> variants; // one LOD chain per mesh variant (picked at random)
    std::vector<Collider> colliders;                // none / one shared / one per variant
    uint32_t colliderUserValue = 0;                 // RaycastHit::userValue of this layer's colliders
    std::string colliderSurface;                    // their physics surface ("Wood", "Stone": footsteps, impacts)

    // ---- More "where" ----
    // Biome rules: return the chance (0..1) of keeping a candidate.
    std::function<float(const FoliageSample&)> density;
    // Keep at least `radius` metres from instances of an earlier layer.
    struct Avoid {
        uint32_t layer;
        float radius;
    };
    std::vector<Avoid> avoid;

    // ---- Interaction (see FoliageSystem::setState / FoliageLink) ----
    std::vector<FoliageState> states; // extra looks, states 1..n
    // > 0: instances this close to the FoliageSystem's collider focus become
    // GameObjects (with a FoliageLink) so gameplay code can live on them; they
    // turn back into instances further away.
    float promoteDistance = 0.0f;
    // Creates the GameObject for a promoted instance (e.g. a registered
    // ChoppableTree); null = a plain GameObject. The system positions it and,
    // unless it has its own, adds the instance's mesh and collider.
    std::function<GameObject*(Scene&, const FoliageInstanceInfo&)> promote;
};

class FoliageSystem;

// On every promoted GameObject: which instance it stands for, and the way to
// change it (the change outlives the object: the system keeps it).
class FoliageLink : public Component {
public:
    FoliageInstanceInfo info;
    FoliageSystem* system() const;
    void setState(uint32_t state);
    void setState(const std::string& name); // "" = the default look
    std::string stateName() const;

private:
    friend class FoliageSystem;
    uint64_t m_system = 0; // FoliageSystem object id
};

// An area nothing is placed in: a circle, or a box turned by `yaw` (radians, about Y).
struct FoliageExclusionShape {
    glm::vec2 center{0.0f};
    float radius = 0.0f;              // circle
    bool box = false;
    glm::vec2 halfExtents{0.0f};      // box (local x, z)
    float yaw = 0.0f;

    // Distance from p to the shape's edge: <= 0 inside.
    float distance(glm::vec2 p) const;
    glm::vec4 bounds() const;         // world x0, z0, x1, z1
    bool operator==(const FoliageExclusionShape&) const = default;
};

struct FoliageSettings {
    float cellSize = 64.0f;
    uint32_t instancesPerFrame = 6000; // budget for adding instances to groups
    uint32_t bodiesPerFrame = 300;     // budget for creating static bodies
    float unloadMargin = 1.15f;        // unload beyond viewDistance * margin (hysteresis)
    // Loading screens wait for the cells this close to the viewer; farther
    // ones (big trees are drawn kilometres away) keep streaming in, nearest
    // first, once play starts. <= 0: wait for everything in view.
    float readyDistance = 300.0f;
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
    void addExclusion(const FoliageExclusionShape& shape);
    // Replaces every exclusion and places the cells around `area` (x0, z0, x1, z1)
    // again: e.g. a building was placed. Instance states (chopped...) are kept.
    void setExclusions(std::vector<FoliageExclusionShape> shapes, glm::vec4 area);

    // Synchronously loads everything visible from `viewer` (+ colliders).
    void preload(const glm::vec3& viewer);
    // Fraction of the cells within settings.readyDistance (all in view if <= 0)
    // that are fully placed (loading screens).
    float streamProgress() const;

    // The ground changed inside `area` (world x0, z0, x1, z1; e.g. terrain
    // brush edits): new world callbacks and layer densities (same layers, in
    // order; empty function = keep), and the cells around the area are placed
    // again. Everything else, and instance states, stay as they are.
    void refresh(FoliageWorld world, const std::vector<std::function<float(const FoliageSample&)>>& densities, glm::vec4 area);

    // Colliders are streamed around this object (e.g. the player); 0 / not
    // found = the camera. Stored by id: the object may be deleted at any time.
    uint64_t colliderFocusId = 0;
    void setColliderFocus(const GameObject* object) { colliderFocusId = object ? object->id() : 0; }

    void update(float dt) override;
    void onGui() override;
    void onDestroy() override;
    const FoliageStats& stats() const { return m_stats; }

    uint32_t layerCount() const { return uint32_t(m_layers.size()); }
    // Instances layer `layer` would place near `p` (deterministic, works for unloaded cells too).
    std::vector<glm::vec3> query(uint32_t layer, glm::vec2 p, float radius) const;

    // ---- Interaction: per-instance state that survives streaming ----
    // The nearest instance of `layer` within `radius` (also in unloaded cells).
    std::optional<FoliageInstanceInfo> nearest(uint32_t layer, glm::vec2 p, float radius) const;
    std::optional<FoliageInstanceInfo> instance(FoliageInstanceId id) const; // null if the rules place nothing there
    uint32_t state(FoliageInstanceId id) const;
    // Switches an instance's look (and collider) now and whenever its cell streams in again.
    void setState(FoliageInstanceId id, uint32_t state);
    void remove(FoliageInstanceId id) { setState(id, kFoliageRemoved); }
    int stateIndex(uint32_t layer, const std::string& name) const; // "" -> 0; -1 = unknown
    const std::string& stateName(uint32_t layer, uint32_t state) const;
    // Every changed instance as (id.packed(), state): store it in a save game, give it back after loading.
    std::vector<std::pair<uint64_t, uint32_t>> overrides() const;
    void setOverrides(const std::vector<std::pair<uint64_t, uint32_t>>& states);
    // The GameObject standing in for an instance while it's promoted (else null).
    GameObject* promoted(FoliageInstanceId id) const;

    // ---- Obstacles (navmesh building) ----
    // Every instance inside `area` (world x0, z0, x1, z1) whose current state
    // has a collider, with its world transform. Works for unloaded cells too.
    void forEachCollider(glm::vec4 area, const std::function<void(const Collider&, const glm::mat4&)>& fn) const;
    // Called when an instance changes state (chopped, removed, restored): e.g.
    // a navmesh rebuilds around `position`. Returns an id for removeStateListener.
    using StateListener = std::function<void(FoliageInstanceId id, const glm::vec3& position)>;
    uint32_t addStateListener(StateListener fn);
    void removeStateListener(uint32_t id);

private:
    struct Instance {
        int gx, gz; // placement grid square (the id)
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
        std::vector<std::vector<std::shared_ptr<InstanceGroup>>> groups; // [state][variant]; extra states created on first use
        uint32_t capacity = 0;                                          // per base variant group
        uint32_t capacityWarned = 0;
    };
    // What became of one generated instance.
    struct Slot {
        uint32_t state = 0;
        int group = -1; // index into the state's groups, -1 = not drawn
        InstanceGroup::Handle handle = InstanceGroup::kInvalid;
        uint32_t body = 0;
        bool hasBody = false;
        uint64_t promoted = 0; // GameObject id while promoted
    };
    enum class CellState { Queued, Ready, Loaded };
    struct Cell {
        uint32_t layer;
        int cx, cz;
        uint32_t ticket;
        CellState state = CellState::Queued;
        std::vector<Instance> instances;
        std::vector<Slot> slots;   // per applied instance
        size_t applied = 0;        // instances added so far
        size_t bodiesCreated = 0;  // instances [0, bodiesCreated) had their collider turn
        uint32_t bodies = 0;       // live bodies
        bool wantBodies = false;
    };
    struct Job {
        uint64_t key;
        uint32_t ticket;
        uint32_t layer;
        int cx, cz;
        float distance; // to the viewer when the queue was last sorted
    };
    struct Result {
        uint64_t key;
        uint32_t ticket;
        std::vector<Instance> instances;
        float ms;
    };

    static uint64_t cellKey(uint32_t layer, int cx, int cz);
    bool candidate(uint32_t layer, int gx, int gz, Candidate* out) const;
    bool makeInstance(uint32_t layer, int gx, int gz, Instance* out) const;
    int cellOf(uint32_t layer, int g) const; // cell column/row holding grid square g
    Cell* loadedCell(FoliageInstanceId id, size_t* index);
    InstanceGroup* groupFor(LayerData& layer, uint32_t state, uint32_t variant, int* index);
    const std::vector<Collider>& collidersFor(const LayerData& layer, uint32_t state) const;
    void showInstance(Cell& cell, size_t i);
    void hideInstance(Cell& cell, size_t i);
    void createBody(Cell& cell, size_t i);
    void destroyBody(Cell& cell, size_t i);
    void promote(Cell& cell, size_t i);
    void demote(Cell& cell, size_t i);
    void updatePromotion(glm::vec2 focus);
    std::vector<Instance> generate(uint32_t layer, int cx, int cz) const;
    float cellDistance(int cx, int cz, glm::vec2 p) const;
    bool cellInBounds(int cx, int cz) const;
    glm::ivec4 cellRange(int x0, int z0, int x1, int z1) const;
    void ensureGroups(LayerData& layer);
    void stopWorkers();
    void replaceCells(glm::vec4 area); // unload the cells near area (and queued ones) to be placed again
    void drain(const std::atomic<bool>& cancelled); // a pool job: generates queued cells, nearest first
    size_t applyInstances(Cell& cell, size_t budget);
    void unload(Cell& cell);
    void destroyBodies(Cell& cell);
    glm::vec3 viewerPosition() const;
    glm::vec3 focusPosition(const glm::vec3& viewer) const;

    FoliageWorld m_world;
    FoliageSettings m_settings;
    std::vector<LayerData> m_layers;
    std::vector<FoliageExclusionShape> m_exclusions;
    std::unordered_map<uint64_t, Cell> m_cells;
    std::unordered_map<uint64_t, uint32_t> m_overrides; // instance id -> state (main thread only)
    std::vector<FoliageInstanceId> m_promoted;          // instances currently stood in for by GameObjects
    std::vector<std::pair<uint32_t, StateListener>> m_stateListeners;
    uint32_t m_nextListener = 1;
    uint32_t m_nextTicket = 1;
    // update() only redoes the expensive walks over every cell when something
    // they depend on changed (big worlds have tens of thousands of cells).
    glm::vec2 m_scanViewer{1e30f}, m_scanFocus{1e30f};
    bool m_rescan = true;        // layers / preload changed the cell set
    bool m_readyWaiting = false; // ready cells not fully added (instance budget)
    bool m_bodiesWaiting = false; // colliders not all created (body budget)
    FoliageStats m_stats;

    // Placement jobs (up to one per pool worker). They call generate() on this
    // system, so the destructor and refresh() cancel *and wait* for them (a
    // cell at most). m_jobs, m_results and m_quit are shared with them.
    jobs::DrainGroup m_drains;
    std::mutex m_mutex;
    std::vector<Job> m_jobs; // sorted farthest-first: drains pop the nearest from the back
    std::vector<Result> m_results;
    bool m_quit = false;
};

} // namespace ve
