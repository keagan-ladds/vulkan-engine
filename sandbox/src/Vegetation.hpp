#pragma once

// =============================================================================
// Procedural low-poly props (acacia / baobab / fever trees, bushes, rocks),
// their placement rules (ve::FoliageSystem layers) and GPU grass/reeds
// (ve::GrassPass). All props share one vertex-coloured StandardMaterial; the
// grass uses the engine's grass_field.vert with the sandbox's grass.frag.
// =============================================================================

#include <ve/physics/Physics.hpp>
#include <ve/render/Instancing.hpp>
#include <ve/scene/GameObject.hpp>

#include <memory>
#include <vector>

namespace ve {
class Terrain;
class Mesh;
class Material;
class InstanceGroup;
class FoliageSystem;
class Renderer;
class Scene;
} // namespace ve

namespace savanna {


// Mesh variants of one kind of prop plus their physics shapes:
// no colliders = walk through it, one = shared by all variants, else one per variant.
struct PropSet {
    // Per variant: its LOD chain (meshes + materials + switch thresholds):
    // full mesh, simplified meshes, and for trees an octahedral impostor last.
    std::vector<std::vector<ve::InstanceLod>> variants;
    std::vector<ve::Collider> colliders;
};

struct PropLibrary {
    PropSet acacias, baobabs, feverTrees, bushes, rocks, boulders;
    std::shared_ptr<ve::Material> material;
};

// Display name for RaycastHit::userValue of a prop collider.
const char* propName(uint32_t userValue);

PropLibrary buildPropLibrary(ve::Renderer& renderer);
// Adds a ve::FoliageSystem with one layer per prop type (biome rules, streaming, colliders).
ve::FoliageSystem& scatterProps(ve::Scene& scene, const ve::Terrain& terrain, const PropLibrary& lib);
// Adds GPU grass + reeds (ve::GrassPass x2).
void addGrass(ve::Renderer& renderer, const ve::Terrain& terrain, std::vector<glm::vec2> clearings = {}); // clearings: 3 m circles without grass

} // namespace savanna
