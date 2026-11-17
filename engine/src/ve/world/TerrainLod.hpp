#pragma once

// =============================================================================
// TerrainLod - quadtree level of detail for ve::Terrain (engine internal).
//
// The world is covered by a quadtree of nodes. Every node is a 33 x 33 vertex
// grid: level 0 nodes sample every grid cell (chunkCells x cellSize metres
// wide), level L nodes every 2^L-th sample over 2^L times the area. Each frame
// the tree is walked from the root: a node splits into its four children
// while the camera is closer than lodFactor x its size, and its children all
// have meshes. So the coarsest available level always covers the ground:
// there are no holes while finer meshes are still being built.
//
// Seams:
//   - Geomorphing: every vertex also stores where it would be on its parent's
//     coarser grid (tangent.x = height, tangent.yzw = normal). terrain.vert
//     moves vertices there as the camera distance approaches the range at
//     which the node merges into its parent, so switching levels doesn't pop.
//   - Skirts: a short wall hangs down from every node edge and hides the
//     small cracks between neighbours of different levels.
//
// Meshes are built on a worker thread (from the pure terrain generator, or the
// tile cache for the finest levels) and uploaded under a per-frame budget.
// After a terrain edit, nodes keep their old mesh until the new one is ready.
// =============================================================================

#include "ve/render/Mesh.hpp"
#include "ve/world/Terrain.hpp"

#include <glm/glm.hpp>

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ve {

class Renderer;

class TerrainLod {
public:
    struct Settings {
        int nodeCells = 32;          // cells per node side
        float lodFactor = 2.5f;      // split within lodFactor x node size
        size_t maxNodes = 1500;      // meshes kept (least recently drawn are freed beyond this)
        uint32_t uploadsPerFrame = 12;
    };
    struct Drawn {
        std::shared_ptr<Mesh> mesh;
        int level;
    };
    struct Stats {
        uint32_t drawn = 0, cached = 0, pending = 0, built = 0;
        float buildMs = 0.0f; // worker time per node, smoothed
    };

    explicit TerrainLod(Renderer& renderer);
    ~TerrainLod();

    // New terrain (or edited): nodes rebuild lazily and keep their old mesh meanwhile.
    // `changed` (world x0, z0, x1, z1): only brush edits there; other nodes keep their meshes.
    void setData(std::shared_ptr<const TerrainData> data, const TerrainLook& look, const Settings& settings,
                 const glm::vec4* changed = nullptr);
    // Per frame: choose nodes for `camera`, queue missing meshes, upload finished ones.
    // preload = build everything needed for this view first (synchronously).
    void update(const glm::vec3& camera, bool preload);

    const std::vector<Drawn>& drawn() const { return m_drawn; }
    int levels() const { return m_levels; }
    // The distance range over which a level-L node morphs into its parent (shader params).
    glm::vec2 morphRange(int level) const;
    const Stats& stats() const { return m_stats; }

private:
    struct Key {
        int level, x, z;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        size_t operator()(const Key& k) const
        {
            return (size_t(uint32_t(k.x)) * 73856093u) ^ (size_t(uint32_t(k.z)) * 19349663u) ^ (size_t(k.level) * 83492791u);
        }
    };
    struct Node {
        std::shared_ptr<Mesh> mesh;
        uint64_t version = 0;  // data version the mesh was built from
        float minY = 0.0f, maxY = 0.0f;
        uint64_t lastUse = 0;
        bool queued = false;
    };
    struct Job {
        Key key;
        uint64_t version;
        int nodeCells;
        std::shared_ptr<const TerrainData> data;
        TerrainLook look;
    };
    struct Result {
        Key key;
        uint64_t version;
        MeshData mesh;
        float minY, maxY;
    };

    float nodeSize(int level) const; // metres
    bool exists(const Key& k) const;  // overlaps the world
    void select(const Key& k, const glm::vec3& camera, std::vector<Key>& want);
    void request(const Key& k, float priority);
    MeshData build(const Job& job, float& minY, float& maxY) const;
    void workerLoop();
    void upload(Result& r);
    void evict();

    Renderer* m_renderer;
    std::shared_ptr<const TerrainData> m_data;
    TerrainLook m_look;
    Settings m_settings;
    uint64_t m_version = 0;
    int m_levels = 1;
    uint64_t m_frame = 0;
    std::unordered_map<Key, Node, KeyHash> m_nodes;
    std::vector<Drawn> m_drawn;
    std::vector<std::pair<float, Key>> m_requests; // this frame's missing meshes, by priority
    Stats m_stats;

    // Worker
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Job> m_jobs;       // nearest / coarsest first
    std::vector<Result> m_results;
    int m_busy = 0;
    bool m_quit = false;
    std::thread m_worker;
};

} // namespace ve
