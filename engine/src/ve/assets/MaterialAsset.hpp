#pragma once

// =============================================================================
// MaterialAsset - a material described by a JSON file (*.material), shared by
// every model that uses it.
//
//   materials/concrete.material
//   {
//     "version": 1,
//     "albedo": "textures/concrete_albedo.png",
//     "normal": "textures/concrete_normal.png",
//     "metallicRoughness": "textures/concrete_orm.png",   // glTF: G = roughness, B = metallic
//     "occlusion": "textures/concrete_orm.png",           // R = AO (same file = one ORM texture)
//     "roughness": 1.0, "metallic": 1.0,                  // multiply the texture
//     "uvScale": [2, 2]
//   }
//
// How models pick them up (AssetManager::loadModel): a glTF material uses a
// material file instead of its own textures and values when
//   1. it has a custom property (glTF extras) "material" naming the file
//      (Blender: Material Properties > Custom Properties; export with
//      Include > Custom Properties), or
//   2. a file named after it exists in AssetManager::materialFolder
//      ("materials/Concrete.material" or "materials/concrete.material" for a
//      material called "Concrete").
// Every model using the file then shares ONE Material: one set of GPU
// textures, one place to tune it, and static renderers of different models
// can share instance batches. From code: assets().loadMaterial(path).
//
// Live: editing the file on disk, or in the editor (Assets > Materials),
// updates every model using it within half a second. Switching a material
// that static renderers use to or from Transparent needs a level reload.
//
// Custom shaders: `fragment` (and optionally `vertex`) replace standard.frag /
// standard.vert but keep StandardMaterial's layout (params 0-3, textures 0-4),
// so a copy of standard.frag works as a starting point. Extra inputs for it:
//   "textures": [{"slot": 5, "path": "textures/grunge.png", "srgb": false}],   // slots 5-15
//   "params":   [{"slot": 4, "value": [1, 0.5, 0, 0]}]                          // slots 4-11
// =============================================================================

#include "ve/render/Material.hpp"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <memory>
#include <string>

namespace ve {

class AssetManager;
class Renderer;
class TypeRegistry;

struct MaterialAsset {
    using Json = nlohmann::json;

    StandardMaterial::Mode mode = StandardMaterial::Mode::Opaque; // Cutout: alpha test, double sided
    std::string vertexShader;   // empty = standard.vert
    std::string fragmentShader; // empty = standard.frag

    glm::vec4 baseColor{1.0f};
    float roughness = 0.8f;
    float metallic = 0.0f;
    float normalStrength = 1.0f;
    float alphaCutoff = 0.5f; // Cutout only
    glm::vec3 emissive{0.0f};
    float emissiveIntensity = 1.0f;
    glm::vec2 uvScale{1.0f};
    float vertexColorWeight = 1.0f; // multiply albedo by vertex colour (0 = ignore it, e.g. colours used as paint masks)
    bool receiveDecals = true;

    // Asset paths (empty = none). Colour textures load as sRGB, the rest linear.
    std::string albedo;
    std::string normal;
    std::string metallicRoughness; // G = roughness, B = metallic
    std::string occlusion;         // R = AO
    std::string emissiveTexture;

    Json textures = Json::array(); // extra slots for custom shaders: [{"slot", "path", "srgb"}]
    Json params = Json::array();   // extra slots for custom shaders: [{"slot", "value": [x, y, z, w]}]

    static MaterialAsset fromJson(const Json& j);
    Json toJson() const;
    // Sets `material` up as described (shader, values, textures loaded through `assets`).
    void apply(StandardMaterial& material, AssetManager& assets) const;
};

// Registers MaterialAsset with the reflection (editor asset window, JSON fields).
void registerAssetTypes(TypeRegistry& registry);

} // namespace ve
