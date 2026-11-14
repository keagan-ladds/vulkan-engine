#pragma once

// =============================================================================
// Model - an imported glTF file: meshes, their materials, textures and the
// node hierarchy. It's an *asset*; put it in the world with
// scene.instantiate(*model).
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

struct ModelNode {
    std::string name;
    glm::mat4 local{1.0f};
    int mesh = -1; // index into Model::meshes, or -1
    std::vector<int> children;
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
};

} // namespace ve
