#pragma once

// =============================================================================
// Vegetation as scene data: trees, bushes, rocks and grass that you set up in
// the editor and that follow the terrain.
//
//   Foliage (GameObject)             streamed instanced props (ve::FoliageSystem)
//     Acacias  [FoliageType]         one layer: mesh, LODs, biome densities, spacing...
//     Bushes   [FoliageType]         avoid = "Acacias:2.5"
//   GrassLayer (GameObject)          GPU grass (ve::GrassPass): blades, or a glTF LOD chain
//   Hut        [FoliageExclusion]    keeps foliage / grass out of a circle
//
// Meshes come from:
//   - a glTF file with an LOD chain (see LodModel.hpp: *_LOD0, *_LOD1...,
//     *_COL colliders; or "lod0.glb; lod1.glb");
//   - or a MeshLibrary entry: meshes a game builds in code, registered by name
//     before the scene loads:
//       ve::MeshLibrary::add("acacia", [](ve::Renderer& r) { ...; return set; });
//
// Densities per biome come from the Terrain's biomes: "Savannah:1, Swamp:0.2".
// Negative values push a layer out of a biome, and "*" is a constant: "*:1,
// Swamp:-1" = everywhere except the swamp (biome weights add up to 1).
//
// Foliage and GrassLayer rebuild (throttled) when the terrain changes
// (Terrain::version()), when their settings change, or when an exclusion
// moves. Both use the first Terrain in the scene.
// =============================================================================

#include "ve/physics/Physics.hpp"
#include "ve/render/Instancing.hpp"
#include "ve/scene/GameObject.hpp"
#include "ve/scene/ObjectRef.hpp"
#include "ve/scene/SaveGame.hpp"
#include "ve/world/Foliage.hpp"

#include <glm/glm.hpp>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ve {

class GrassPass;
class Renderer;
class Terrain;
class TerrainData;

// Ready-to-draw meshes of one kind of prop.
struct FoliageMeshes {
    std::vector<std::vector<InstanceLod>> variants; // one LOD chain per variant
    std::vector<Collider> colliders;                // none / one shared / one per variant
    uint32_t colliderUserValue = 0;                 // RaycastHit::userValue
};

class MeshLibrary {
public:
    using Builder = std::function<FoliageMeshes(Renderer&)>;
    // Registers a builder; it runs the first time a scene asks for `name`.
    static void add(const std::string& name, Builder builder);
    // Built on first use, then cached until releaseMeshes(); null if unknown.
    static std::shared_ptr<const FoliageMeshes> get(Renderer& renderer, const std::string& name);
    static void releaseMeshes(); // GPU meshes go (done at Engine shutdown); builders stay

    static std::vector<std::string> names();
};

// "a:1, b:2" -> {{"a", 1}, {"b", 2}}; a bare name means 1.
std::vector<std::pair<std::string, float>> parseNameValues(const std::string& spec);

// "Savannah:1, Swamp:-0.5" -> weights by biome index of `terrain` ("*" = -1, a
// constant term); unknown names are skipped (warned once).
std::vector<std::pair<int, float>> parseBiomeWeights(const std::string& spec, const TerrainData& terrain,
                                                     const std::string& owner);
// Sum of weight x biome weight at a point, clamped to 0..1; 1 if `weights` is empty.
float biomeDensity(const std::vector<std::pair<int, float>>& weights, const TerrainData& terrain, glm::vec2 p);

// Keeps foliage and/or grass out of a circle, or a box, around its object
// (huts, camps, roads, building foundations). The box turns with the
// object's yaw. Adding, moving or removing one while playing re-places only
// the vegetation around it (foliage keeps chopped / picked states; grass
// re-bakes in the background), so buildings can carry one.
class FoliageExclusion : public Component {
public:
    bool box = false;              // false: a circle of `radius`
    float radius = 3.0f;
    glm::vec2 size{4.0f, 4.0f};    // box: full extents along the object's local x and z
    glm::vec2 offset{0.0f};        // box / circle centre in the object's local x, z
    float falloff = 1.0f;          // grass thins out over this distance
    bool foliage = true;
    bool grass = true;

    FoliageExclusionShape shape() const; // in the world, now
};

// One foliage layer, on a child of a Foliage object. Its object's name is the
// layer name (used by `avoid`).
// The placement / streaming fields (spacing, scale, viewDistance...) come
// from FoliageRules, the same struct FoliageLayer uses.
class FoliageType : public Component, public FoliageRules {
public:
    FoliageType();

    enum class Collision { Auto, None, Capsule, Cylinder, Sphere, Box, ConvexHull };
    enum class LodMode { ScreenSize, Distance };

    // ---- Meshes ----
    std::string mesh;    // glTF path(s); or
    std::string library; // a MeshLibrary name
    LodMode lodMode = LodMode::ScreenSize;
    // Switch points LOD0->1, 1->2, 2->3: screen size (fraction of half the
    // screen height covered by the bounding radius) or distance in m.
    glm::vec3 lodThresholds{0.10f, 0.035f, 0.012f};
    int autoLods = 0;       // glTF with one level: add this many simplified levels (0..2)
    bool impostor = false;  // glTF: an octahedral impostor as the last level
    Collision collision = Collision::Auto; // Auto = library / *_COL nodes / none
    glm::vec3 colliderSize{0.3f, 3.0f, 0.3f}; // capsule/cylinder: radius, height; sphere: radius; box: size
    int colliderUserValue = 0;
    std::string colliderSurface; // physics surface of the colliders (footsteps, impacts)

    // ---- Where ----
    std::string biomes;          // "Savannah:1, Swamp:0.2" (empty = everywhere)
    float biomeThreshold = 0.0f; // 0 = density is a probability; >0 = all-or-nothing at this density
    std::string avoid;           // "Acacias:2.5, Rocks:1" (earlier layers of the same Foliage)

    // ---- Interaction (FoliageSystem::setState, FoliageLink) ----
    // Extra looks: "Stump = acacia stump; Picked = models/bush_picked.glb"
    // (a MeshLibrary name, or glTF path(s) loaded like `mesh`; empty = invisible).
    std::string states;
    float promoteDistance = 0.0f; // > 0: instances this close to the player become GameObjects...
    std::string promoteType;      // ...of this registered type (empty = a plain GameObject)

    // Meshes for the current settings (cached while mesh / LOD settings stay the same).
    std::shared_ptr<const FoliageMeshes> meshes(Renderer& renderer);
    // The extra states' meshes, in `states` order (built by meshes()).
    const std::vector<std::pair<std::string, std::shared_ptr<const FoliageMeshes>>>& stateMeshes() const { return m_stateMeshes; }

private:
    std::shared_ptr<const FoliageMeshes> buildMeshes(Renderer& renderer, const std::string& mesh, const std::string& library,
                                                     const std::string& owner);
    std::string m_meshKey;
    std::shared_ptr<const FoliageMeshes> m_meshes;
    std::vector<std::pair<std::string, std::shared_ptr<const FoliageMeshes>>> m_stateMeshes;
};

class Foliage : public GameObject, public Saveable {
public:
    Foliage();
    // Save games: every instance whose state changed (chopped trees, picked bushes).
    void saveState(Json& out) const override;
    void loadState(const Json& in) override;
    void start() override;
    void update(float dt) override;

    float edgeMargin = 45.0f;            // keep this far inside the terrain's edges
    float cellSize = 64.0f;
    int instancesPerFrame = 6000;
    int bodiesPerFrame = 300;
    // Colliders stream around the object with this name (else the camera).
    // "Player" is only a naming convention (the sandbox's player object is
    // called that); set your own player's name, or "" for the camera.
    std::string colliderFocus = "Player";
    bool preload = true;                 // load what's visible synchronously on the first build

    void markDirty() { m_dirty = true; }
    FoliageSystem* system() const { return m_system.get(scene()); } // null before the first build
    // Layer index of a FoliageType by its object's name (for FoliageSystem::query), -1 if none.
    int layerIndex(const std::string& name) const;

private:
    struct LayerDensity { // to rebind a layer's density to a new terrain snapshot
        std::vector<std::pair<int, float>> weights;
        float threshold = 0.0f;
    };
    std::string state() const; // what needs a rebuild (exclusions are handled incrementally)
    void rebuild();
    void refresh(glm::vec4 area); // terrain brush edits: re-place around `area` only
    FoliageWorld makeWorld(const std::shared_ptr<const TerrainData>& td) const;
    static std::function<float(const FoliageSample&)> makeDensity(const std::shared_ptr<const TerrainData>& td, const LayerDensity& d);

    ObjectRef<FoliageSystem> m_system; // a hidden child (the editor could still delete it)
    std::vector<std::pair<uint64_t, uint32_t>> m_pendingOverrides; // loaded before the system existed
    bool m_hasPendingOverrides = false;
    std::vector<std::string> m_layerNames;
    std::vector<LayerDensity> m_densities; // per system layer
    std::string m_state;
    std::vector<FoliageExclusionShape> m_exclusions; // as last applied
    uint64_t m_terrainVersion = 0;
    ObjectRef<const Terrain> m_terrain; // built against (compared by id: it may be deleted)
    bool m_dirty = true;
    bool m_built = false;
    float m_sinceBuild = 1e6f;
    float m_sinceCheck = 1e6f; // since state() / exclusions / focus were last compared
};

// GPU grass over the terrain (one ve::GrassPass).
class GrassLayer : public GameObject {
public:
    GrassLayer();
    ~GrassLayer() override;
    void start() override;
    void update(float dt) override;
    void onDestroy() override;

    // ---- Look (empty mesh = procedural blades) ----
    std::string mesh;             // glTF LOD chain (*_LOD0, *_LOD1..., or "a.glb; b.glb"), variant 0
    glm::vec3 lodDistances{8.0f, 25.0f, 60.0f}; // switch distances LOD0->1, 1->2, 2->3
    float lodBlend = 4.0f;
    std::string texture;          // overrides the meshes' own base colour texture
    int cards = 0;                // >0 and no mesh: this many crossed quads textured with `texture`
    glm::vec3 colorA{0.55f, 0.6f, 0.25f}; // tints (multiply the texture)
    glm::vec3 colorB{0.75f, 0.65f, 0.35f};
    float alphaCutoff = 0.5f;
    float minHeight = 0.45f, maxHeight = 1.0f;
    float bladeWidth = 0.05f;
    int bladesPerClump = 4;
    float clumpRadius = 0.25f;
    float maxTilt = 0.2f;
    float sway = 1.0f;
    std::string fragmentShader;   // blades only (empty = engine default)

    // ---- Where ----
    std::string biomes;           // "Savannah:1, Camp:1" (empty = everywhere)
    float biomeThreshold = 0.0f;  // >0: hard cut-off instead of thinning
    float minGroundHeight = -1e6f, maxGroundHeight = 1e6f;
    float maxSlope = 1.0f;        // 1 - normal.y
    float patchFrequency = 0.0f;  // noise patches (0 = off): density *= clamp(fbm * contrast - offset)
    float patchContrast = 2.0f;
    float patchOffset = 0.2f;
    float colorFrequency = 0.012f; // colour A/B mix noise
    float heightFrequency = 0.05f; // height variation noise
    float heightVariation = 0.3f;  // 0 = all full height
    int seed = 0;

    // ---- Density / distance ----
    float spacing = 0.8f;
    float radius = 110.0f;
    float fadeStart = 30.0f;
    float farDensity = 0.3f;
    float maskResolution = 1.0f;  // mask texels per metre
    bool castShadows = true;
    float shadowDistance = 30.0f;

    void markDirty() { m_dirty = true; }
    GrassPass* pass() const { return m_pass; }

private:
    std::string state() const; // what needs a rebuild (exclusions are handled incrementally)
    void rebuild();
    void removePass();
    struct Exclusion {
        FoliageExclusionShape shape;
        float falloff; // grass fades in over this distance from the edge
        bool operator==(const Exclusion&) const = default;
    };
    std::vector<Exclusion> exclusions() const; // the scene's active grass exclusions
    std::function<glm::vec4(glm::vec2)> makeMask(const std::shared_ptr<const TerrainData>& td, std::vector<Exclusion> exclusions) const;

    GrassPass* m_pass = nullptr;
    std::string m_passName;
    std::string m_state;
    std::vector<Exclusion> m_exclusions; // as last applied
    uint64_t m_terrainVersion = 0;
    ObjectRef<const Terrain> m_terrain; // built against (compared by id: it may be deleted)
    bool m_dirty = true;
    float m_sinceBuild = 1e6f;
    float m_sinceCheck = 1e6f; // since state() / exclusions were last compared
};

class TypeRegistry;
// Reflection (scene files, editor): this file's components. The Engine calls it at startup.
void registerVegetationTypes(TypeRegistry& r);

} // namespace ve
