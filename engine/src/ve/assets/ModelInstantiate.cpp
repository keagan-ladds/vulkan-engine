// Scene::instantiate (declared in ve/scene/Scene.hpp): a model's node tree as
// GameObjects. It lives with the assets, not in scene/, because it builds
// components of the modules above the scene (anim's SkinnedMeshRenderer and
// Animator, physics' Rigidbody); scene itself doesn't include them.

#include "ve/anim/Animation.hpp"
#include "ve/assets/Model.hpp"
#include "ve/physics/Physics.hpp"
#include "ve/scene/Components.hpp"
#include "ve/scene/Scene.hpp"

#include <glm/gtx/matrix_decompose.hpp>

#include <functional>

namespace ve {

GameObject& Scene::instantiate(const Model& model, GameObject* parent, bool staticColliders)
{
    GameObject& root = spawn(model.name.empty() ? std::string("Model") : model.name);
    if (parent) root.setParent(parent, false);

    // Recursively create one GameObject per glTF node.
    std::vector<GameObject*> objects(model.nodes.size(), nullptr);
    std::function<void(int, GameObject&)> build = [&](int nodeIndex, GameObject& parentObj) {
        const ModelNode& node = model.nodes[nodeIndex];
        GameObject& go = spawn(node.name.empty() ? std::string("Node") : node.name);
        objects[size_t(nodeIndex)] = &go;
        go.setParent(&parentObj, false);

        glm::vec3 scale, translation, skew;
        glm::vec4 perspective;
        glm::quat rotation;
        glm::decompose(node.local, scale, rotation, translation, skew, perspective);
        go.transform().setLocalPosition(translation);
        go.transform().setLocalRotation(rotation);
        go.transform().setLocalScale(scale);

        if (node.mesh >= 0 && node.skin >= 0) {
            // Bound below, once every joint object exists.
            auto& skinned = go.addComponent<SkinnedMeshRenderer>();
            skinned.materials = model.meshMaterials[node.mesh];
        } else if (node.mesh >= 0) {
            go.addComponent<MeshRenderer>(model.meshes[node.mesh], model.meshMaterials[node.mesh]);
            if (staticColliders) {
                Collider c = EngineAccess::physics().meshCollider(model.meshes[node.mesh], model.meshData[node.mesh]);
                go.addComponent<Rigidbody>(c, BodyType::Static);
            }
        }
        for (int child : node.children) build(child, go);
    };
    for (int r : model.roots) build(r, root);

    // Skeletal animation: skinned meshes follow their joint objects, an Animator plays the clips.
    for (size_t n = 0; n < model.nodes.size(); ++n) {
        const ModelNode& node = model.nodes[n];
        if (node.skin < 0 || !objects[n]) continue;
        const ModelSkin& skin = model.skins[size_t(node.skin)];
        std::vector<GameObject*> joints;
        for (int j : skin.joints) joints.push_back(j >= 0 && size_t(j) < objects.size() ? objects[size_t(j)] : nullptr);
        if (auto* s = objects[n]->getComponent<SkinnedMeshRenderer>())
            s->bind(model.skinnedMeshes[size_t(node.mesh)], joints, skin.inverseBind);
    }
    if (!model.animations.empty()) root.addComponent<Animator>().bind(model.animations, objects);
    return root;
}

} // namespace ve
