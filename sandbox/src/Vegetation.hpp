#pragma once

// =============================================================================
// Procedural low-poly props (acacia / baobab / fever trees, bushes, rocks),
// wind-swept grass and reeds, and the functions that scatter them over the
// terrain. All props share one vertex-coloured StandardMaterial; the grass
// uses its own shader (grass.vert/frag) that sways with the global wind.
// =============================================================================

#include <ve/physics/Physics.hpp>
#include <ve/scene/GameObject.hpp>

#include <memory>
#include <vector>

namespace ve {
class Mesh;
class Material;
class InstanceGroup;
class Renderer;
class Scene;
} // namespace ve

namespace savanna {

class Terrain;

// Mesh variants of one kind of prop plus their physics shapes:
// no colliders = walk through it, one = shared by all variants, else one per variant.
struct PropSet {
    std::vector<std::shared_ptr<ve::Mesh>> meshes;                // LOD 0 per variant
    std::vector<std::vector<std::shared_ptr<ve::Mesh>>> lods;     // LOD 1.. per variant (simplified)
    std::vector<float> screenSizes;                               // LOD switch thresholds (InstanceLod::screenSize)
    std::vector<ve::Collider> colliders;
};

struct PropLibrary {
    PropSet acacias, baobabs, feverTrees, bushes, rocks, boulders;
    std::shared_ptr<ve::Material> material;
};

// Owns the instanced props (GPU instance groups + static physics bodies) so
// they live and die with the scene.
class PropField : public ve::GameObject {
public:
    PropField() : GameObject("Props") {}
    ~PropField() override;
    std::vector<std::shared_ptr<ve::InstanceGroup>> groups;
    std::vector<uint32_t> bodies;
};

// Display name for RaycastHit::userValue of a prop collider.
const char* propName(uint32_t userValue);

PropLibrary buildPropLibrary(ve::Renderer& renderer);
void scatterProps(ve::Scene& scene, const Terrain& terrain, const PropLibrary& lib);
void buildGrass(ve::Scene& scene, ve::Renderer& renderer, const Terrain& terrain);

} // namespace savanna
