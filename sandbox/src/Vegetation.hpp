#pragma once

// =============================================================================
// Procedural low-poly props (acacia / baobab / fever trees, bushes, rocks),
// wind-swept grass and reeds, and the functions that scatter them over the
// terrain. All props share one vertex-coloured StandardMaterial; the grass
// uses its own shader (grass.vert/frag) that sways with the global wind.
// =============================================================================

#include <ve/physics/Physics.hpp>

#include <memory>
#include <vector>

namespace ve {
class Mesh;
class Material;
class Renderer;
class Scene;
} // namespace ve

namespace savanna {

class Terrain;

// Mesh variants of one kind of prop plus their physics shapes:
// no colliders = walk through it, one = shared by all variants, else one per variant.
struct PropSet {
    std::vector<std::shared_ptr<ve::Mesh>> meshes;
    std::vector<ve::Collider> colliders;
};

struct PropLibrary {
    PropSet acacias, baobabs, feverTrees, bushes, rocks, boulders;
    std::shared_ptr<ve::Material> material;
};

PropLibrary buildPropLibrary(ve::Renderer& renderer);
void scatterProps(ve::Scene& scene, const Terrain& terrain, const PropLibrary& lib);
void buildGrass(ve::Scene& scene, ve::Renderer& renderer, const Terrain& terrain);

} // namespace savanna
