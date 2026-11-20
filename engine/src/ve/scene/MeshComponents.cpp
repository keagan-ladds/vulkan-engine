#include "ve/scene/MeshComponents.hpp"

#include "ve/anim/Animation.hpp"

#include "ve/assets/AssetManager.hpp"
#include "ve/assets/Model.hpp"
#include "ve/core/Log.hpp"
#include "ve/core/Strings.hpp"
#include "ve/render/Material.hpp"
#include "ve/render/Mesh.hpp"
#include "ve/render/Primitives.hpp"
#include "ve/render/Renderer.hpp"
#include "ve/scene/Components.hpp"
#include "ve/scene/Scene.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <tuple>

namespace ve {

// -----------------------------------------------------------------------------
// ModelInstance
// -----------------------------------------------------------------------------

GameObject* ModelInstance::instanceRoot() const
{
    return m_root.get(gameObject().scene());
}

void ModelInstance::rebuild()
{
    if (GameObject* old = instanceRoot()) old->destroy();
    m_root.reset();
    if (path.empty()) return;
    try {
        auto model = assets().loadModel(path);
        GameObject* root = &gameObject().scene().instantiate(*model, &gameObject(), staticColliders);
        m_root = root;
        if (!hiddenNodes.empty()) {
            const std::vector<std::string> names = str::split(hiddenNodes, ',');
            // Destroyed before they start: no colliders or draws for hidden nodes.
            std::function<void(Transform&)> hide = [&](Transform& t) {
                for (Transform* c : t.children()) {
                    if (std::find(names.begin(), names.end(), c->gameObject().name()) != names.end()) c->gameObject().destroy();
                    else hide(*c);
                }
            };
            hide(root->transform());
        }
        if (auto* animator = root->getComponent<Animator>(); animator && !animation.empty()) animator->clip = animation;
    } catch (const std::exception& e) {
        log::warn("ModelInstance on '{}': {}", gameObject().name(), e.what());
    }
}

void ModelInstance::onDestroy()
{
    // The instantiated nodes are children: they go with the owner. When only
    // this component is removed, remove them too.
    if (GameObject* root = instanceRoot(); root && !gameObject().isDestroyed()) root->destroy();
    m_root.reset();
}

// -----------------------------------------------------------------------------
// PrimitiveRenderer
// -----------------------------------------------------------------------------

void PrimitiveRenderer::rebuild()
{
    Renderer& r = renderer();
    const glm::vec3 s = glm::max(size, glm::vec3(0.001f));
    // Primitives of the same shape and size share one mesh (a crate pyramid is
    // one cube), kept alive by the renderers using it.
    static std::map<std::tuple<Shape, float, float, float>, std::weak_ptr<Mesh>> shared;
    const std::tuple<Shape, float, float, float> key{shape, s.x, s.y, s.z};
    auto found = shared.find(key);
    std::shared_ptr<Mesh> mesh = found != shared.end() ? found->second.lock() : nullptr;
    if (!mesh) {
        // About to add one: drop entries whose meshes are gone (sizes edited
        // in the editor would otherwise pile up).
        std::erase_if(shared, [](const auto& entry) { return entry.second.expired(); });
        MeshData md;
        switch (shape) {
        case Shape::Cube: md = primitives::cube(s); break;
        case Shape::Sphere: md = primitives::sphere(s.x * 0.5f); break;
        case Shape::Cylinder: md = primitives::cylinder(s.x * 0.5f, s.x * 0.5f, s.y, 24); break;
        case Shape::Plane: md = primitives::plane(s.x, s.z); break;
        }
        mesh = Mesh::create(r, md, {}, "primitive");
        shared[key] = mesh;
    }
    if (!m_material) m_material = r.createStandardMaterial();
    m_material->setBaseColor(color).setRoughness(roughness).setMetallic(metallic).setUvScale(uvScale);
    std::shared_ptr<Texture> tex;
    if (!texture.empty()) {
        try {
            tex = assets().loadTexture(texture);
        } catch (const std::exception& e) {
            log::warn("PrimitiveRenderer on '{}': {}", gameObject().name(), e.what());
        }
    }
    m_material->setAlbedoTexture(tex);
    MeshRenderer* mr = m_renderer.get(gameObject().scene());
    if (!mr) m_renderer = mr = &gameObject().addComponent<MeshRenderer>();
    mr->mesh = mesh;
    mr->materials = {m_material};
    mr->castShadows = castShadows;
}

void PrimitiveRenderer::onDestroy()
{
    if (MeshRenderer* mr = m_renderer.get(gameObject().scene()); mr && !gameObject().isDestroyed()) gameObject().removeComponent(mr);
    m_renderer.reset();
}

} // namespace ve
