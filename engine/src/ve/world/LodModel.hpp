#pragma once

// =============================================================================
// LodModel - LOD chains authored in glTF (Blender), for foliage and grass.
//
//   auto lods = ve::loadLodModel(assets, "models/acacia.gltf");
//   for (auto& variant : lods.variants)        // one per distinct object
//       for (auto& level : variant.levels) ... // nearest first
//
// How a file is read:
//   - Nodes named `<name>_LOD0`, `<name>_LOD1`... (or just `LOD0`, `LOD1`...)
//     are the levels of variant `<name>`. A level node may be an empty with
//     mesh children: they are merged. Blender's `.001` suffixes are ignored.
//   - Nodes named `<name>_COL` / `COL` / `UCX_<name>` are collision shapes
//     (their convex hull), not drawn.
//   - Without LOD names, every root node with meshes under it is a variant
//     with one level ("rock_a, rock_b, rock_c" in one file).
//   - A level's position is ignored, its rotation and scale kept: in Blender
//     you can lay the levels out side by side.
//   - Several files separated by ';' ("grass_lod0.glb; grass_lod1.glb") are
//     the levels in order; variant i of each file goes to variant i.
//
// buildLodChain() turns levels (authored and/or simplified from level 0) into
// the InstanceLod chain InstanceGroups / FoliageLayers draw:
//
//   ve::LodChainDesc c;
//   c.name = "tree";
//   c.levels = {{treeMesh, {}, {material}}};
//   c.simplify = {{.ratio = 0.35f, .maxError = 0.1f, .faceted = true}};
//   c.impostor = true;                                  // octahedral card last
//   c.switches = {{.screenSize = 0.12f}, {.screenSize = 0.04f}};
//   std::vector<ve::InstanceLod> lods = ve::buildLodChain(renderer, std::move(c));
// =============================================================================

#include "ve/render/Impostor.hpp"
#include "ve/render/Instancing.hpp"
#include "ve/render/Mesh.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ve {

class AssetManager;
class Impostor;
struct ModelData;
struct ModelSettings;
struct ModelVariantLods;
struct ModelImpostorData;
class Material;
class Model;
class Renderer;

/// One LOD level's geometry, before upload.
struct LodGeometry {
    MeshData data;  ///< The level's mesh data.
    std::vector<Submesh> submeshes;                    ///< materialSlot indexes `materials`
    std::vector<std::shared_ptr<Material>> materials;  ///< Materials per submesh slot.
};

/// One variant of a model (a different tree), with its LOD levels.
struct LodVariant {
    std::string name;  ///< Variant name (from the glTF).
    std::vector<LodGeometry> levels; ///< nearest first
    MeshData collider;               ///< merged *_COL shapes (empty = none authored)
    /// What cooking generated for this variant (simplified levels, an
    /// impostor; ModelSettings), or null. Valid while the LodModel is.
    const ModelVariantLods* cooked = nullptr;
};

/// A glTF LOD model: variants x levels (see the top of this file; loadLodModel).
struct LodModel {
    std::vector<LodVariant> variants;  ///< The variants found in the file.
    std::shared_ptr<const std::vector<ModelVariantLods>> cooked; ///< Keeps LodVariant::cooked alive.
    uint32_t levelCount() const;     ///< most levels of any variant
};

/// Throws if a file can't be loaded or holds no meshes.
LodModel loadLodModel(AssetManager& assets, const std::string& paths);
LodModel lodModelFrom(const Model& model);

/// What buildLodChain uploads.
struct LodChainDesc {
    std::string name;                     ///< mesh debug names: "`<name>` LOD<i>"
    std::vector<LodGeometry> levels;      ///< nearest first; at least one
    /// More levels simplified (meshoptimizer) from levels[0], appended after
    /// `levels` (only for a single-submesh level 0). They use its materials.
    std::vector<SimplifyOptions> simplify;
    size_t maxLevels = SIZE_MAX;          ///< mesh levels kept (the impostor doesn't count)
    bool impostor = false;                ///< an octahedral impostor of level 0 as the last level (baked now)
    /// A cooked impostor (ModelSettings::impostor) to use as the last level
    /// instead of baking one; wins over `impostor`. Must outlive the call.
    const ModelImpostorData* cookedImpostor = nullptr;
    ImpostorSettings impostorSettings;    ///< its bake settings (atlas size, edge colour...)
    /// Switch thresholds per level (impostor included): only screenSize and
    /// maxDistance are read; levels without an entry get InstanceLod's defaults.
    std::vector<InstanceLod> switches;
};

/// Optimizes and uploads every level (and bakes the impostor). Shared by the
/// FoliageType scene component, games' hand-built props and the templates.
std::vector<InstanceLod> buildLodChain(Renderer& renderer, LodChainDesc desc);

/// Generates the LOD levels and impostors a model's settings ask for, into
/// `data.lods` (the model cooker's world-side step: see ModelCook.hpp). Needs
/// the Engine's renderer (main thread) for impostors.
bool generateModelLods(ModelData& data, const Model& model, const ModelSettings& settings, std::string& error);
/// Registers generateModelLods with the model cooker (the Engine does at startup).
void registerModelLodGenerator();
/// An impostor from cooked atlases (null if the GPU can't use their format).
std::shared_ptr<Impostor> impostorFromCooked(Renderer& renderer, const ModelImpostorData& data);

/// "LOD3" -> 3 (and base name), -1 if the name has no LOD suffix.
int parseLodSuffix(const std::string& nodeName, std::string* base = nullptr);

} // namespace ve
