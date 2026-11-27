#pragma once

// =============================================================================
// ModelData - a model as plain CPU data: what a glTF file holds once parsed
// (meshes with tangents and normals already computed, materials as
// descriptions, nodes, skins, animations, embedded images), plus what cooking
// adds (generated LOD levels and impostor atlases, see ModelSettings).
//
//   ModelData d = parseGltf("models/hut.gltf");           // CPU only: any thread, vecook
//   auto model = buildModel(renderer, d, &assets, "models/hut.gltf", settings);   // GPU: main thread
//
// The cooked form (`<cache>/<asset path>.vemodel`, ModelCook.hpp) is this
// struct written with write() / read(): loading a cooked model is reading
// arrays and uploading them, with no glTF parsing.
//
// ModelSettings come from an optional `<model>.model` JSON file next to the
// model (like `.texture` for images):
//   { "autoLods": 2, "impostor": true, "materials": { "Bark": "materials/acacia_bark.material" } }
// =============================================================================

#include "ve/anim/Animation.hpp"
#include "ve/assets/Model.hpp"
#include "ve/core/BinaryIO.hpp"
#include "ve/render/Mesh.hpp"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ve {

class AssetManager;
class Renderer;

/// What cooking does with a model beyond converting it: generated LODs,
/// an impostor, material remapping. From `<model>.model`; defaults otherwise.
struct ModelSettings {
    int autoLods = 0;               ///< Simplified levels added to variants that have a single level (0..3).
    float lodRatio = 0.35f;         ///< Each generated level keeps this fraction of the previous level's triangles.
    float lodMaxError = 0.08f;      ///< Allowed deviation per generated level (relative to the mesh size; doubles per level).
    bool lodFaceted = false;        ///< Low-poly flat-shaded meshes: keep them faceted when simplifying.
    bool impostor = false;          ///< Bake an octahedral impostor of LOD0 as the last level of every variant.
    uint32_t impostorFrames = 12;   ///< Views per atlas side (frames x frames views).
    uint32_t impostorResolution = 128; ///< Pixels per view.
    float impostorAlphaCutoff = 0.45f; ///< Alpha test threshold of the impostor.
    glm::vec3 impostorBackground{0.3f}; ///< Colour of the atlas's empty texels (near the model's average colour: no dark fringes).
    /// Material slot (the glTF / Blender material name) -> material file. Wins
    /// over the material's "material" custom property and the by-name lookup.
    std::map<std::string, std::string> materials;

    /// Whether cooking generates anything (LOD levels or an impostor).
    bool generatesLods() const { return autoLods > 0 || impostor; }
    /// The settings of a model file: its `.model` file if there is one, else defaults.
    static ModelSettings forModel(const std::filesystem::path& modelFile);
    /// `<model>.model`.
    static std::filesystem::path settingsFile(const std::filesystem::path& modelFile);
    nlohmann::json toJson() const;            ///< As a .model file's JSON.
    void applyJson(const nlohmann::json& j);  ///< Sets the fields `j` has (others keep their values).
    uint64_t cookHash() const;                ///< Changes whenever the cooked result would.
};

/// A texture a material uses: an image file next to the model, or an embedded image.
struct ModelTextureRef {
    std::string uri;      ///< File, relative to the model's folder (decoded); empty if embedded or none.
    int embedded = -1;    ///< Index into ModelData::images; -1 = not embedded.
    bool nearest = false; ///< The glTF sampler asks for nearest filtering (pixel art).
    /// Whether a texture is referenced at all.
    bool valid() const { return !uri.empty() || embedded >= 0; }
};

/// A glTF material as data (turned into a StandardMaterial, or replaced by a
/// material file, when the model is built).
struct ModelMaterialData {
    std::string name;                 ///< The material's name (Blender's), used to find material files.
    std::string extras;               ///< glTF extras as JSON text (Blender custom properties), or empty.
    std::string file;                 ///< Material file chosen by the .model remap when cooked (wins over everything); empty = none.
    int mode = 0;                     ///< 0 opaque, 1 cutout (alpha test / double sided), 2 transparent.
    float alphaCutoff = 0.0f;         ///< Cutout threshold (0 = no alpha test).
    glm::vec4 baseColor{1.0f};        ///< Base colour factor (linear RGBA).
    float roughness = 1.0f;           ///< Roughness factor.
    float metallic = 1.0f;            ///< Metallic factor.
    float normalScale = 1.0f;         ///< Normal map strength.
    glm::vec3 emissive{0.0f};         ///< Emissive factor (linear RGB).
    float emissiveStrength = 1.0f;    ///< KHR_materials_emissive_strength.
    ModelTextureRef albedo;           ///< Base colour texture.
    ModelTextureRef metallicRoughness;///< G = roughness, B = metallic.
    ModelTextureRef normal;           ///< Normal map.
    ModelTextureRef occlusion;        ///< Ambient occlusion (R).
    ModelTextureRef emissiveTexture;  ///< Emission colour.
};

/// An image stored in the model file (.glb, data: URIs).
struct ModelImageData {
    std::string name;            ///< Its name in the file.
    std::vector<uint8_t> bytes;  ///< The encoded image (PNG / JPEG).
};

/// One mesh: geometry, its submeshes' materials and (optionally) skinning data.
struct ModelMeshData {
    std::string name;                     ///< The mesh's name.
    MeshData data;                        ///< Vertices (normals and tangents computed) and indices.
    std::vector<Submesh> submeshes;       ///< Index ranges; materialSlot indexes `materials`.
    std::vector<int> materials;           ///< Per slot: index into ModelData::materials (-1 = a default material).
    std::vector<glm::uvec4> joints;       ///< Skinned meshes: per vertex, 4 joint indices into its skin.
    std::vector<glm::vec4> weights;       ///< Skinned meshes: per vertex, 4 weights summing to 1.
    std::vector<float> jointRadius;       ///< Skinned meshes: per joint, distance of its farthest weighted vertex.
    /// Whether the mesh has any triangles.
    bool empty() const { return data.indices.empty(); }
    /// Whether it is deformed by joints.
    bool skinned() const { return !joints.empty(); }
};

/// LOD levels generated by cooking, for one variant of a model (LodModel.hpp).
struct ModelLodLevelData {
    MeshData data;                   ///< The simplified mesh (optimized for the GPU).
    std::vector<Submesh> submeshes;  ///< materialSlot indexes the variant's LOD0 materials.
};

/// A baked impostor's atlases: block-compressed, every mip level.
struct ModelImpostorData {
    uint32_t albedoFormat = 0;        ///< VkFormat of the colour atlas (BC7 sRGB).
    uint32_t normalFormat = 0;        ///< VkFormat of the normal atlas (BC7).
    uint32_t size = 0;                ///< Atlas width = height in pixels (mip 0).
    std::vector<uint64_t> levelOffsets; ///< Byte offset of each mip, the same in both buffers.
    std::vector<uint8_t> albedo;      ///< Colour atlas, every mip.
    std::vector<uint8_t> normal;      ///< Normal atlas, every mip.
    glm::vec3 centre{0.0f};           ///< Framing: bounding-sphere centre in mesh space.
    float radius = 1.0f;              ///< Framing: bounding-sphere radius.
    uint32_t frames = 0;              ///< Views per atlas side.
    float alphaCutoff = 0.45f;        ///< Alpha test threshold.
    /// Whether there is an impostor.
    bool valid() const { return size > 0; }
};

/// What cooking generated for one variant (matched by name with LodModel's variants).
struct ModelVariantLods {
    std::string variant;                       ///< The variant's name.
    std::vector<ModelLodLevelData> generated;  ///< Simplified levels, after the authored ones.
    ModelImpostorData impostor;                ///< The impostor (invalid if none).
};

/// A model as CPU data (see the top of this file).
struct ModelData {
    std::string name;                           ///< The file's stem.
    std::vector<ModelMeshData> meshes;          ///< ModelNode::mesh indexes these.
    std::vector<ModelMaterialData> materials;   ///< The file's materials.
    std::vector<ModelImageData> images;         ///< Embedded images.
    std::vector<ModelNode> nodes;               ///< The node hierarchy.
    std::vector<int> roots;                     ///< Top-level nodes.
    std::vector<ModelSkin> skins;               ///< Skeletons.
    std::vector<AnimationClip> animations;      ///< Animation clips.
    std::vector<ModelVariantLods> lods;         ///< Cooked LOD levels / impostors (empty when none were generated).
    /// Files the model was made from besides the main file (external buffers),
    /// relative to its folder: cooking checks them, packaging leaves them out.
    std::vector<std::string> dependencies;

    void write(BinaryWriter& out) const;  ///< Appends the model to a binary stream.
    bool read(BinaryReader& in);          ///< Reads a model written by write(); false if truncated.
};

/// Parses a glTF 2.0 file (.gltf with external / embedded buffers, or .glb)
/// into ModelData: flat normals where the file has none, tangents where it
/// has UVs. CPU only (any thread; reads through ve::vfs). Throws on errors.
ModelData parseGltf(const std::filesystem::path& file);

/// Builds a GPU Model from ModelData (main thread).
/// @param assets for textures and material files (null: textures from the
///        model's folder, no material files).
/// @param modelPath the model's path as loaded (an asset path or a file):
///        texture URIs are relative to its folder.
/// @param settings material remapping (ModelSettings::materials).
/// @param dependencies if given, receives the material files the build used.
std::shared_ptr<Model> buildModel(Renderer& renderer, const ModelData& data, AssetManager* assets, const std::filesystem::path& modelPath,
                                  const ModelSettings& settings = {}, std::vector<std::filesystem::path>* dependencies = nullptr);

} // namespace ve
