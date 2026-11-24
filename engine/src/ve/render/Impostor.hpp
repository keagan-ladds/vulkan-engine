#pragma once

// =============================================================================
// Octahedral impostors: a far-away LOD that draws a whole tree (or any mesh)
// as one camera-facing quad.
//
// Baking renders the mesh from N x N directions over the upper hemisphere
// (hemi-octahedral layout) into an albedo atlas and an object-space normal
// atlas. At runtime the impostor shader blends the 3 frames nearest the view
// direction and lights the result with the normal engine lighting, so it
// matches day/night and receives shadows. A matching shadow shader makes
// impostors cast shadows facing the light.
//
//   auto imp = ve::Impostor::bake(renderer, *treeMesh, {treeMaterial});
//   desc.lods.push_back(imp->lod(/*screenSize*/ 0.0f));   // last LOD of an InstanceGroup
//
// Baking reads the StandardMaterial parameter layout (base colour, albedo
// texture, vertex colour weight, alpha cutoff). It takes a few milliseconds
// per mesh, at load time.
// =============================================================================

#include "ve/render/Instancing.hpp"

#include <memory>
#include <vector>

namespace ve {

class Renderer;
class Mesh;
class Material;
class Texture;

/// How an impostor is baked.
struct ImpostorSettings {
    uint32_t frames = 12;          ///< frames per atlas side (N x N views)
    uint32_t frameResolution = 128; ///< pixels per frame
    float alphaCutoff = 0.45f;     ///< Alpha below this is cut out when drawing.
    /// Albedo of the atlas's empty texels (alpha 0). Mips and bilinear filtering
    /// blend it into the silhouette's edge, so set it near the object's average
    /// colour (e.g. the foliage tint) to avoid dark or off-colour fringes.
    glm::vec3 backgroundAlbedo{0.3f, 0.3f, 0.3f};
};

/// An octahedral impostor: a mesh rendered from many directions into an atlas,
/// drawn far away as one camera-facing quad picking the closest views.
class Impostor {
public:
    /// Renders `mesh` (with its materials) into albedo / normal atlases on the
    /// GPU and waits for it (load time only).
    static std::shared_ptr<Impostor> bake(Renderer& renderer, const Mesh& mesh,
                                          const std::vector<std::shared_ptr<Material>>& materials,
                                          const ImpostorSettings& settings = {});

    /// Ready-made LOD level for an InstanceGroup.
    InstanceLod lod(float screenSize = 0.0f, float maxDistance = FLT_MAX) const
    {
        return {m_quad, {m_material}, screenSize, maxDistance};
    }

    const std::shared_ptr<Mesh>& mesh() const { return m_quad; }               ///< The quad it's drawn with.
    const std::shared_ptr<Material>& material() const { return m_material; }   ///< Its material (atlases + view count).
    const std::shared_ptr<Texture>& albedoAtlas() const { return m_albedo; }   ///< Colour atlas (alpha = coverage).
    const std::shared_ptr<Texture>& normalAtlas() const { return m_normal; }   ///< Normal atlas.

private:
    std::shared_ptr<Mesh> m_quad;
    std::shared_ptr<Material> m_material;
    std::shared_ptr<Texture> m_albedo, m_normal;
};

} // namespace ve
