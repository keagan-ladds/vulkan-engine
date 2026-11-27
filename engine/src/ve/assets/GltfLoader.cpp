// =============================================================================
// glTF 2.0 importer: parseGltf (ModelData.cpp, CPU) + buildModel (GPU).
//
// glTF concepts -> engine concepts:
//   mesh (+ primitives)   -> Mesh with one Submesh per primitive
//   material              -> StandardMaterial (metallic-roughness PBR), or a material file
//   image/texture         -> Texture (sRGB for colour, linear for data)
//   node hierarchy        -> ModelNode tree (Scene::instantiate makes GameObjects)
// Cooked models (ModelCook.hpp) skip the parsing: AssetManager::loadModel
// reads their ModelData and calls buildModel directly.
// =============================================================================

#include "ve/assets/AssetManager.hpp"
#include "ve/assets/ModelData.hpp"

namespace ve {

std::shared_ptr<Model> loadGltf(Renderer& renderer, const std::filesystem::path& file, AssetManager* assets)
{
    const ModelSettings settings = ModelSettings::forModel(file);
    return buildModel(renderer, parseGltf(file), assets, file, settings);
}

} // namespace ve
