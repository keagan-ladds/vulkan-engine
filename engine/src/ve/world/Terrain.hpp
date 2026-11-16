#pragma once

// =============================================================================
// Terrain - a procedural heightfield with biomes, as a scene object.
//
// Height = base noise + waves + stamps (in hierarchy order) + border mountains.
// TerrainStamp components (usually on child objects you place with the gizmo)
// flatten areas to their height (camps, roads, lakes with noisy pools) or raise
// hills and ridges, and can paint a biome. Biomes (see TerrainBiome) colour the
// ground and tell vegetation what grows where; their weights are blended at
// the borders and can be queried anywhere (biomeWeight()).
//
// The terrain rebuilds itself when its fields or any stamp change (throttled
// while dragging in the editor); systems that sample it (foliage, grass) watch
// version(). Queries are thread-safe (they read an immutable snapshot).
//
//   ve::Terrain* t = ve::Terrain::find(scene);
//   float h = t->height(x, z);
//   float swamp = t->biomeWeight("Swamp", x, z);
// =============================================================================

#include "ve/scene/GameObject.hpp"
#include "ve/scene/Reflection.hpp"

#include <glm/glm.hpp>

#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <string>
#include <vector>

namespace ve {

class Material;
class Scene;

struct TerrainBiome {
    std::string name = "Biome";
    // Ground colour: color mixed towards color2 by noise, plus an optional patch colour.
    glm::vec3 color{0.5f, 0.5f, 0.3f};
    glm::vec3 color2{0.5f, 0.5f, 0.3f};
    float colorNoiseFrequency = 0.012f;
    int colorNoiseSeed = 31;
    glm::vec3 patchColor{0.5f};
    float patchFrequency = 0.0f; // 0 = no patches
    int patchSeed = 41;
    float patchCoverage = 0.35f;
    float patchStrength = 0.85f;
    // Where it occurs on its own (stamps can paint it anywhere): height range,
    // max slope (1 - normal.y), and noise patches covering `coverage` of the
    // area (frequency 0 = everywhere the other rules allow). Biome 0 fills the rest.
    float minHeight = -1e9f, maxHeight = 1e9f;
    float maxSlope = 1.0f;
    float coverage = 0.0f;       // 0 = only where stamps paint it
    float coverageFrequency = 0.004f;
    int coverageSeed = 61;
};

// The shape of a TerrainStamp (plain data, so terrain snapshots can copy it).
struct TerrainStampShape {
    enum class Mode { Flatten, Raise };
    Mode mode = Mode::Flatten;
    float radius = 10.0f;   // full effect inside
    float falloff = 10.0f;  // blends out over this distance
    // Flatten: blend towards this object's height (+ noise pools).
    float noiseAmplitude = 0.0f;
    float noiseFrequency = 0.025f;
    int noiseSeed = 7;
    // Raise: height * profile^exponent (+ ridged noise). Negative = a pit.
    float height = 10.0f;
    float exponent = 1.5f;
    float ridgeAmount = 0.0f;
    float ridgeFrequency = 0.03f;
    int ridgeSeed = 11;
    // Paint a biome (by name) with its own radius / falloff.
    std::string biome;
    float biomeRadius = 10.0f;
    float biomeFalloff = 10.0f;
    float biomeStrength = 1.0f;
};

class TerrainStamp : public Component, public TerrainStampShape {};

// The terrain's shape parameters (plain data, copied into every TerrainData).
struct TerrainShape {
    int seed = 1;
    float size = 640.0f;        // world extent (centred on the object), m
    float cellSize = 2.0f;      // m between height samples
    float baseHeight = 2.5f;
    float noiseFrequency = 0.0035f;
    float noiseAmplitude = 16.0f;
    int noiseOctaves = 5;
    glm::vec3 waves{1.5f, 0.011f, 0.013f}; // amplitude, x / z frequency
    float borderStart = 200.0f; // mountains rise from this distance from the centre...
    float borderEnd = 315.0f;   // ...to full height here
    float borderHeight = 55.0f;
    float borderNoise = 60.0f;
    float borderNoiseFrequency = 0.009f;
};

// An immutable snapshot of a terrain: its generator inputs plus a cache of
// sample tiles generated on demand, so worlds of any size cost memory only
// where something looks (rendering, physics, foliage, grass, audio).
// All queries are thread-safe; keep the shared_ptr while using it.
//
// Samples lie on a grid of `res` x `res` points `cellSize` apart from
// `origin`. Heights between samples follow the render mesh's triangles.
class TerrainData {
public:
    static constexpr int kTileCells = 64; // cells per cache tile side (65 x 65 samples)

    TerrainData(const TerrainShape& shape, glm::vec3 centre, std::vector<TerrainBiome> biomes,
                std::vector<std::pair<glm::vec3, TerrainStampShape>> stamps, size_t cacheBytes);
    TerrainData(const TerrainData&) = delete;
    TerrainData& operator=(const TerrainData&) = delete;

    glm::vec2 origin{0.0f}; // world x/z of sample (0, 0)
    float cellSize = 2.0f;
    int res = 0;            // samples per side
    std::vector<std::string> biomeNames;

    float height(float x, float z) const;
    glm::vec3 normal(float x, float z) const;
    float biomeWeight(int biome, float x, float z) const;
    int biomeIndex(const std::string& name) const;
    bool inBounds(float x, float z, float margin = 0.0f) const;
    glm::vec2 boundsMax() const { return origin + glm::vec2(float(res - 1) * cellSize); }

    // Grid samples (indices clamped to the grid).
    float sampleHeight(int i, int j) const;
    float sampleBiome(int biome, int i, int j) const;
    glm::vec2 samplePosition(int i, int j) const { return origin + glm::vec2(float(i), float(j)) * cellSize; }

    // The generator itself: height at any world point (exact at sample points).
    float generateHeight(glm::vec2 p) const;

    size_t cachedTiles() const;

private:
    struct Tile {
        std::vector<float> heights;              // 65 x 65
        std::vector<std::vector<float>> biomes;  // [biome][65 x 65]
    };
    struct Stamp {
        glm::vec3 pos;
        TerrainStampShape s;
        int biome; // -1 = none
    };
    std::shared_ptr<const Tile> tile(int tx, int tz) const;
    std::shared_ptr<const Tile> generateTile(int tx, int tz) const;
    // Tile and local sample index holding grid sample (i, j) (clamped).
    std::shared_ptr<const Tile> tileFor(int& i, int& j) const;

    TerrainShape m_shape;
    glm::vec3 m_centre;
    std::vector<TerrainBiome> m_biomes;
    std::vector<Stamp> m_stamps;
    uint64_t m_uid; // tells thread-local lookup caches apart

    mutable std::mutex m_mutex;
    struct Cached {
        std::shared_ptr<const Tile> tile;
        uint64_t lastUse;
    };
    mutable std::unordered_map<int64_t, Cached> m_tiles;
    mutable uint64_t m_clock = 0;
    size_t m_maxTiles;
};

// Shape fields (seed, size, cellSize, noise, border...) come from TerrainShape.
class Terrain : public GameObject, public TerrainShape {
public:
    Terrain();
    ~Terrain() override;
    void start() override;
    void update(float dt) override;
    void onDestroy() override;

    // ---- Shape: see TerrainShape ----
    int chunkCells = 32;        // cells per render chunk side
    float waterLevel = 0.0f;
    float tileCacheMB = 96.0f;  // sample tiles kept in memory (regenerated on demand beyond that)

    // ---- Look ----
    glm::vec3 rockColor{0.50f, 0.46f, 0.41f};
    glm::vec3 mudColor{0.23f, 0.18f, 0.12f};
    float steepStart = 0.3f, steepEnd = 0.5f; // slope (1 - n.y) where rock takes over
    float borderRockStart = 230.0f, borderRockEnd = 300.0f, borderRock = 0.6f;
    float brightnessVariation = 0.2f;
    std::string detailTexture = "textures/ground_detail.png";
    glm::vec2 detailScales{0.31f, 0.043f};
    int wetnessSlot = 1; // frame.globalParams slot whose .x is wetness (effects' weather); -1 none

    std::vector<TerrainBiome> biomes{TerrainBiome{}};

    // ---- Queries (thread-safe) ----
    std::shared_ptr<const TerrainData> data() const; // builds on first use
    float height(float x, float z) const { return data()->height(x, z); }
    glm::vec3 normal(float x, float z) const { return data()->normal(x, z); }
    float slope(float x, float z) const { return 1.0f - normal(x, z).y; }
    float biomeWeight(const std::string& biome, float x, float z) const;
    bool inBounds(float x, float z, float margin = 0.0f) const { return data()->inBounds(x, z, margin); }
    glm::vec2 boundsMin() const;
    glm::vec2 boundsMax() const;
    uint64_t version() const { return m_version; } // bumps on every rebuild

    void markDirty() { m_dirty = true; }
    static Terrain* find(const Scene& scene);

    // Biomes as JSON (reflection / scene files).
    Json biomesJson() const;
    void setBiomesJson(const Json& j);

private:
    std::shared_ptr<TerrainData> generate() const;
    Json editState() const; // own fields + position + stamps: rebuild when it changes
    void rebuild();      // data + meshes + collider
    void buildVisuals(); // chunk meshes + heightfield collider from m_data

    mutable std::mutex m_mutex;
    mutable std::shared_ptr<const TerrainData> m_data;
    std::shared_ptr<Material> m_material;
    std::vector<GameObject*> m_chunks;
    Component* m_collider = nullptr;
    std::string m_editState;
    uint64_t m_version = 0;
    bool m_dirty = false;
    bool m_started = false;
    float m_sinceRebuild = 0.0f;
    float m_sinceCheck = 0.0f; // edit-state checks are throttled (they serialize the fields)
};

} // namespace ve
