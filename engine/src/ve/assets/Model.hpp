#pragma once

// =============================================================================
// Model - an imported glTF file: meshes, their materials, textures, the node
// hierarchy, skins and animation clips. It's an *asset*; put it in the world
// with scene.instantiate(*model) (skinned meshes get a SkinnedMeshRenderer,
// animated models an Animator; see ve/anim/Animation.hpp).
// =============================================================================

#include "ve/physics/Physics.hpp"
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

struct ModelNode {
    std::string name;
    glm::mat4 local{1.0f};
    int mesh = -1; // index into Model::meshes, or -1
    int skin = -1; // index into Model::skins: the mesh is deformed by those joints
    std::vector<int> children;
};

struct ModelSkin {
    std::string name;
    std::vector<int> joints;            // node indices
    std::vector<glm::mat4> inverseBind; // per joint
};

class Model {
public:
    std::string name;
    std::vector<std::shared_ptr<Mesh>> meshes;
    // meshMaterials[m][slot] = material for submeshes of mesh m with materialSlot == slot
    std::vector<std::vector<std::shared_ptr<Material>>> meshMaterials;
    std::vector<std::shared_ptr<Texture>> textures;
    // CPU copy of each mesh's geometry (for colliders, picking, ...).
    std::vector<MeshData> meshData;
    // Lazily built static mesh colliders, shared by every instance of the model.
    mutable std::vector<Collider> meshColliders;
    std::vector<ModelNode> nodes;
    std::vector<int> roots;
    std::vector<ModelSkin> skins;
    // Per mesh: its skinned version (rest pose + joint weights), null if it has no joints.
    std::vector<std::shared_ptr<const SkinnedMeshSource>> skinnedMeshes;
    std::vector<std::shared_ptr<const AnimationClip>> animations;
};

} // namespace ve
