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
// =============================================================================

#include "ve/render/Mesh.hpp"

#include <memory>
#include <string>
#include <vector>

namespace ve {

class AssetManager;
class Material;
class Model;

struct LodGeometry {
    MeshData data;
    std::vector<Submesh> submeshes;                    // materialSlot indexes `materials`
    std::vector<std::shared_ptr<Material>> materials;
};

struct LodVariant {
    std::string name;
    std::vector<LodGeometry> levels; // nearest first
    MeshData collider;               // merged *_COL shapes (empty = none authored)
};

struct LodModel {
    std::vector<LodVariant> variants;
    uint32_t levelCount() const;     // most levels of any variant
};

// Throws if a file can't be loaded or holds no meshes.
LodModel loadLodModel(AssetManager& assets, const std::string& paths);
LodModel lodModelFrom(const Model& model);

// "LOD3" -> 3 (and base name), -1 if the name has no LOD suffix.
int parseLodSuffix(const std::string& nodeName, std::string* base = nullptr);

} // namespace ve
