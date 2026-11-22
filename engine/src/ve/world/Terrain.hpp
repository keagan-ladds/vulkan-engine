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
#include "ve/scene/ObjectRef.hpp"
#include "ve/scene/Reflection.hpp"
#include "ve/scene/SaveGame.hpp"

#include <glm/glm.hpp>

#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <string>
#include <vector>

namespace ve {

class Material;
class MeshRenderer;
class Scene;
class Shader;
class TerrainLod;
class Texture;

// A tiling ground material (splat layer). Every texture is optional: without
// an albedo the layer keeps the biome's vertex colour but can still add normal,
// roughness and AO detail. Normal maps use the OpenGL convention (green = up).
// The albedo's alpha, if any, is a height used to sharpen the borders between
// layers (sand fills the gaps between stones...). Normal, roughness and AO are
// packed into one texture when the terrain loads them.
struct TerrainMaterial {
    std::string albedo, normal, roughness, ao; // asset paths
    float size = 4.0f;            // metres per texture repeat
    glm::vec3 tint{1.0f};         // albedo multiplier
    float normalStrength = 1.0f;
    float roughnessValue = 0.9f;  // without a roughness map
    // 0 = the albedo's own colours; 1 = the biome's colour (with its noise and
    // patches) carrying the albedo's light/dark detail. Keeps a biome's palette
    // art-directable while the texture adds pebbles, cracks, straw...
    float biomeColor = 0.0f;
    bool textured() const { return !albedo.empty() || !normal.empty() || !roughness.empty() || !ao.empty(); }
};

// Something that grows in a biome: a foliage (".foliage") or grass (".grass")
// asset file and its density there (0..1; summed over the biomes at a point,
// weighted by how much of each biome is there).
struct BiomePlant {
    std::string type;     // asset path, e.g. "foliage/acacia.foliage"
    float density = 1.0f;
    bool operator==(const BiomePlant&) const = default;
};

// A biome: how the ground looks, where it occurs, and what grows on it.
// Usually its own asset file ("biomes/savannah.biome", JSON: the fields below
// plus "foliage" / "grass" lists) that terrains reference, so several scenes
// can share it and the editor can edit it live (see docs/06-world.md).
struct TerrainBiome {
    std::string name = "Biome";
    // The .biome file it comes from (empty = defined in the scene file). Not
    // part of the file itself.
    std::string asset;
    // What grows here (BiomeVegetation turns these into foliage layers / grass).
    std::vector<BiomePlant> foliage;
    std::vector<BiomePlant> grass;
    // Optional textured ground (otherwise the colours below, with detail noise).
    TerrainMaterial material;
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

// A biome's content as JSON (the .biome file format: every field except
// `asset`), and back. Missing keys keep their defaults; a missing name
// becomes `fallbackName` (biome files: the file name).
Json biomeToJson(const TerrainBiome& biome);
TerrainBiome biomeFromJson(const Json& json, const std::string& fallbackName = "Biome");
// A biome loaded from its .biome file (DataAssets: cached, reloaded on change).
TerrainBiome loadBiomeAsset(const std::string& path);

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

// How the ground is coloured (plain data: the mesh builder thread copies it).
struct TerrainLook {
    float waterLevel = 0.0f;
    glm::vec3 rockColor{0.50f, 0.46f, 0.41f};
    glm::vec3 mudColor{0.23f, 0.18f, 0.12f};
    float steepStart = 0.3f, steepEnd = 0.5f; // slope (1 - n.y) where rock takes over
    float borderRockStart = 230.0f, borderRockEnd = 300.0f, borderRock = 0.6f;
    float brightnessVariation = 0.2f;
    // Optional texture for rock (steep slopes, border mountains), projected
    // from three sides so cliffs don't stretch it.
    TerrainMaterial rockMaterial;
};

// Tile coordinates <-> one hash-map key (z in the high 32 bits, x in the low):
// shared by the edit tiles, the sample cache and the collider tiles.
inline int64_t terrainTileKey(int tx, int tz) { return (int64_t(tz) << 32) | int64_t(uint32_t(tx)); }
inline glm::ivec2 terrainTileCoords(int64_t key) { return {int(uint32_t(key)), int(key >> 32)}; }

// Hand-made changes on top of the generator (the editor's sculpt and paint
// brushes): a height offset and a painted biome per grid sample, stored
// sparsely in tiles of 64 cells (only where something was edited). Sample
// coordinates are relative to the terrain's centre sample, so edits stay put
// when the terrain is resized or moved. Tiles are shared between snapshots
// (copy-on-write when brushing).
struct TerrainEdits {
    static constexpr int kTile = 64, kSide = kTile + 1; // neighbouring tiles share their border samples
    struct Tile {
        std::vector<float> height = std::vector<float>(kSide * kSide, 0.0f);     // m
        std::vector<uint8_t> biome = std::vector<uint8_t>(kSide * kSide, 255);   // painted biome, 255 = none
        std::vector<uint8_t> paint = std::vector<uint8_t>(kSide * kSide, 0);     // paint strength 0..255
    };
    std::unordered_map<int64_t, std::shared_ptr<const Tile>> tiles;

    bool empty() const { return tiles.empty(); }
    float height(int gi, int gj) const;                         // offset at a sample
    void paint(int gi, int gj, int& biome, float& strength) const; // biome -1 = none
    static int64_t key(int tx, int tz) { return terrainTileKey(tx, tz); }
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
                std::vector<std::pair<glm::vec3, TerrainStampShape>> stamps, size_t cacheBytes, TerrainEdits edits = {});
    // After brush edits only: reuse `old`'s cached sample tiles outside the
    // world-space rectangle `changed` (x0, z0, x1, z1).
    void adoptTiles(const TerrainData& old, glm::vec4 changed);
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

    // The generator itself: height at any world point (exact at sample points),
    // and the biome weights for a point of that height and slope (1 - normal.y).
    float generateHeight(glm::vec2 p) const;  // including edits
    float baseHeight(glm::vec2 p) const;      // the generator alone (no edits)
    float editHeight(glm::vec2 p) const;      // the edits' offset (bilinear between samples)
    const TerrainEdits& edits() const { return m_edits; }
    // Grid sample (i, j) -> edit coordinates (relative to the centre sample).
    glm::ivec2 editCoord(int i, int j) const { return {i - m_half, j - m_half}; }
    void generateBiomes(glm::vec2 p, float height, float slope, float* weights) const; // biomeNames.size() weights
    // Vertex colour of the ground (biome colours, mud, rock) for these weights,
    // and how much of it is rock and mud.
    glm::vec3 groundColor(const TerrainLook& look, glm::vec2 p, float height, glm::vec3 normal, const float* weights,
                          float* rock = nullptr, float* mud = nullptr) const;
    bool biomeTextured(int biome) const { return m_biomes[size_t(biome)].material.textured(); }
    glm::vec3 centre() const { return m_centre; }

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
    TerrainEdits m_edits;
    int m_half = 0; // centre sample index (edit coordinates)
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

// Shape fields (seed, size, cellSize, noise, border...) come from TerrainShape,
// colours (rock, mud, waterLevel...) from TerrainLook.
class Terrain : public GameObject, public TerrainShape, public TerrainLook, public Saveable {
public:
    // Save games: runtime edits (brushes applied by the game).
    void saveState(Json& out) const override { out["edits"] = editsJson(); }
    void loadState(const Json& in) override
    {
        if (in.contains("edits")) setEditsJson(in["edits"]);
    }
    Terrain();
    ~Terrain() override;
    void start() override;
    void update(float dt) override;
    void onDestroy() override;

    // ---- Streaming / LOD ----
    int chunkCells = 32;        // cells per LOD node side (finest level)
    float lodFactor = 2.5f;     // a node splits within lodFactor x its size of the camera (higher = more detail)
    float colliderDistance = 256.0f; // physics tiles around the camera, characters and dynamic bodies
    float tileCacheMB = 96.0f;  // sample tiles kept in memory (regenerated on demand beyond that)

    // ---- Look (see TerrainLook for the colours) ----
    // Greyscale noise that breaks up the ground colours (two scales below).
    // Empty = none (a flat mid-grey stands in, leaving the colours as they are).
    std::string detailTexture;
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

    // ---- Sculpting & painting (the editor's terrain tools; saved with the scene) ----
    struct Brush {
        enum class Mode { Raise, Lower, Smooth, Flatten, Paint };
        Mode mode = Mode::Raise;
        float radius = 6.0f;   // m (soft edge over the outer half)
        float strength = 0.5f; // 0..1
        float height = 0.0f;   // Flatten: target height (world)
        int biome = 0;         // Paint: biome index
    };
    // One brush step of `dt` seconds centred on world (x, z). The terrain
    // rebuilds just that area (throttled), foliage and grass follow.
    void applyBrush(const Brush& brush, glm::vec2 centre, float dt);
    void clearEdits();
    size_t editedTiles() const { return m_edits.tiles.size(); }
    Json editsJson() const;
    void setEditsJson(const Json& j);
    // True if every rebuild after `version` only changed brush edits; `area`
    // gets the world rectangle (x0, z0, x1, z1) they touched. Foliage and
    // grass use it to refresh just that area instead of everything.
    bool changedOnlyIn(uint64_t version, glm::vec4& area) const;
    static Terrain* find(const Scene& scene);

    // Biomes as JSON (reflection / scene files): a biome from a file is just
    // {"asset": "biomes/x.biome"}, others are stored in full.
    Json biomesJson() const;
    void setBiomesJson(const Json& j);
    // Re-reads the biomes that come from files if any data asset changed
    // (called from update(); the editor calls it after editing a biome file).
    void refreshBiomeAssets();
    static Json materialJson(const TerrainMaterial& m);
    static TerrainMaterial materialFromJson(const Json& j);

    // Streaming state (F1 overlay / tests).
    uint32_t drawnNodes() const;
    float loadProgress() const; // LOD meshes the view needs that exist (loading screens)
    size_t colliderTiles() const { return m_colliderTiles.size(); }

private:
    std::shared_ptr<TerrainData> generate(bool quiet = false) const;
    Json editState() const; // own fields + position + stamps: rebuild when it changes
    void rebuild();         // new data snapshot; meshes and colliders follow
    void updateMaterials(); // one material per LOD level (morph range), sharing the look
    void updateNodes(bool preload);
    void updateColliders(bool force, const glm::vec4* area = nullptr);

    mutable std::mutex m_mutex;
    mutable std::shared_ptr<const TerrainData> m_data;
    std::unique_ptr<TerrainLod> m_lod;
    std::shared_ptr<Shader> m_shader;
    std::shared_ptr<Texture> m_flatDetail; // 1x1 mid-grey: no detailTexture
    std::vector<std::shared_ptr<Material>> m_levelMaterials;
    std::shared_ptr<Texture> m_layerTable;                                     // splat layer parameters
    std::vector<float> m_layerTableData;
    std::unordered_map<std::string, std::shared_ptr<Texture>> m_layerTextures; // albedo / packed normal-roughness-AO
    ObjectRef<> m_nodeObject;                     // child holding one MeshRenderer per drawn node
    std::vector<MeshRenderer*> m_nodeRenderers;   // its components (valid while the child is: they die together)
    std::unordered_map<int64_t, Component*> m_colliderTiles; // tile key -> Rigidbody (heightfield)
    float m_sinceColliders = 1e9f;
    std::string m_editState;
    uint64_t m_version = 0;
    TerrainEdits m_edits;
    uint64_t m_editsVersion = 0;
    bool m_editsReplaced = false; // loaded / cleared / undone: rebuild everything
    glm::vec4 m_editArea{1e30f, 1e30f, -1e30f, -1e30f}; // brushed since the last rebuild
    struct Change {
        uint64_t version;
        bool editsOnly;
        glm::vec4 area;
    };
    std::vector<Change> m_changes; // recent rebuilds (changedOnlyIn)
    bool m_dirty = false;
    bool m_started = false;
    bool m_loadTask = false; // registered a loading-screen task (first build)
    float m_sinceRebuild = 0.0f;
    float m_sinceCheck = 0.0f; // edit-state checks are throttled (they serialize the fields)
    uint64_t m_biomeAssetsGeneration = 0; // DataAssets::generation() the biome files were read at
};

class TypeRegistry;
// Reflection (scene files, editor): this file's components. The Engine calls it at startup.
void registerTerrainTypes(TypeRegistry& r);

} // namespace ve
