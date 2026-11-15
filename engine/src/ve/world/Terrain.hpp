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

// Immutable sample grids (shared with worker threads).
struct TerrainData {
    glm::vec2 origin{0.0f}; // world x/z of sample (0, 0)
    float cellSize = 2.0f;
    int res = 0;            // samples per side
    std::vector<float> heights;
    std::vector<std::string> biomeNames;
    std::vector<std::vector<float>> biomeWeights; // [biome][sample]

    float height(float x, float z) const;
    glm::vec3 normal(float x, float z) const;
    float biomeWeight(int biome, float x, float z) const;
    int biomeIndex(const std::string& name) const;
    bool inBounds(float x, float z, float margin = 0.0f) const;
};

class TerrainStamp : public Component {
public:
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

class Terrain : public GameObject {
public:
    Terrain();
    ~Terrain() override;
    void start() override;
    void update(float dt) override;
    void onDestroy() override;

    // ---- Shape ----
    int seed = 1;
    float size = 640.0f;        // world extent (centred on the object), m
    float cellSize = 2.0f;      // m between height samples
    int chunkCells = 32;        // cells per render chunk side
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
    float waterLevel = 0.0f;

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
    Json stampState() const;
    void rebuild();      // data + meshes + collider
    void buildVisuals(); // chunk meshes + heightfield collider from m_data

    mutable std::mutex m_mutex;
    mutable std::shared_ptr<const TerrainData> m_data;
    std::shared_ptr<Material> m_material;
    std::vector<GameObject*> m_chunks;
    Component* m_collider = nullptr;
    std::string m_stampState;
    uint64_t m_version = 0;
    bool m_dirty = false;
    bool m_started = false;
    float m_sinceRebuild = 0.0f;
};

} // namespace ve
