#pragma once

// =============================================================================
// Model - an imported glTF file: meshes, their materials, textures, the node
// hierarchy, skins and animation clips. It's an *asset*; put it in the world
// with scene.instantiate(*model) (skinned meshes get a SkinnedMeshRenderer,
// animated models an Animator; see ve/anim/Animation.hpp).
// =============================================================================

#include "ve/render/Mesh.hpp"

#include <memory>
#include <string>
#include <vector>

namespace ve {

class Mesh;
class Material;
class Texture;
class AnimationClip;
struct SkinnedMeshSource;

/// One node of a model's hierarchy (a glTF node).
struct ModelNode {
    std::string name;          ///< The node's name (from the modelling tool).
    glm::mat4 local{1.0f};     ///< Transform relative to its parent node.
    int mesh = -1; ///< index into Model::meshes, or -1
    int skin = -1; ///< index into Model::skins: the mesh is deformed by those joints
    std::vector<int> children; ///< Indices of its child nodes in Model::nodes.
    std::string extras; ///< the node's glTF "extras" as JSON text (Blender: custom properties), or empty
};

/// A skeleton: the nodes that deform a skinned mesh.
struct ModelSkin {
    std::string name;                   ///< The skin's name.
    std::vector<int> joints;            ///< node indices
    std::vector<glm::mat4> inverseBind; ///< per joint
};

/// An imported model (glTF): meshes, materials, textures, the node hierarchy,
/// skins and animations. AssetManager::loadModel caches one per file; spawn it
/// with Scene::instantiate or a ModelInstance component.
class Model {
public:
    std::string name;                           ///< The file's name.
    std::vector<std::shared_ptr<Mesh>> meshes;  ///< GPU meshes; ModelNode::mesh indexes them.
    /// meshMaterials[m][slot] = material for submeshes of mesh m with materialSlot == slot
    std::vector<std::vector<std::shared_ptr<Material>>> meshMaterials;
    std::vector<std::shared_ptr<Texture>> textures; ///< Textures its materials use (kept alive with the model).
    /// CPU copy of each mesh's geometry (for colliders, picking, ...).
    std::vector<MeshData> meshData;
    std::vector<ModelNode> nodes;  ///< Every node; children refer to each other by index.
    std::vector<int> roots;        ///< Indices of the top-level nodes.
    std::vector<ModelSkin> skins;  ///< Skeletons (ModelNode::skin indexes them).
    /// Per mesh: its skinned version (rest pose + joint weights), null if it has no joints.
    std::vector<std::shared_ptr<const SkinnedMeshSource>> skinnedMeshes;
    std::vector<std::shared_ptr<const AnimationClip>> animations; ///< The file's animation clips.
};

} // namespace ve
