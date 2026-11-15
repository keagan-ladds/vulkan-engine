#include "ve/scene/MeshComponents.hpp"

#include "ve/assets/AssetManager.hpp"
#include "ve/assets/Model.hpp"
#include "ve/core/Log.hpp"
#include "ve/render/Material.hpp"
#include "ve/render/Mesh.hpp"
#include "ve/render/Primitives.hpp"
#include "ve/render/Renderer.hpp"
#include "ve/scene/Components.hpp"
#include "ve/scene/Scene.hpp"

namespace ve {

// -----------------------------------------------------------------------------
// ModelInstance
// -----------------------------------------------------------------------------

void ModelInstance::rebuild()
{
    if (m_root) {
        m_root->destroy();
        m_root = nullptr;
    }
    if (path.empty()) return;
    try {
        auto model = assets().loadModel(path);
        m_root = &gameObject().scene().instantiate(*model, &gameObject(), staticColliders);
    } catch (const std::exception& e) {
        log::warn("ModelInstance on '{}': {}", gameObject().name(), e.what());
    }
}

void ModelInstance::onDestroy()
{
    // The instantiated nodes are children: they go with the owner. When only
    // this component is removed, remove them too.
    if (m_root && !gameObject().isDestroyed()) m_root->destroy();
    m_root = nullptr;
}

// -----------------------------------------------------------------------------
// PrimitiveRenderer
// -----------------------------------------------------------------------------

void PrimitiveRenderer::rebuild()
{
    Renderer& r = renderer();
    const glm::vec3 s = glm::max(size, glm::vec3(0.001f));
    MeshData md;
    switch (shape) {
    case Shape::Cube: md = primitives::cube(s); break;
    case Shape::Sphere: md = primitives::sphere(s.x * 0.5f); break;
    case Shape::Cylinder: md = primitives::cylinder(s.x * 0.5f, s.x * 0.5f, s.y, 24); break;
    case Shape::Plane: md = primitives::plane(s.x, s.z); break;
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
    if (!m_renderer) m_renderer = &gameObject().addComponent<MeshRenderer>();
    m_renderer->mesh = Mesh::create(r, md, {}, "primitive");
    m_renderer->materials = {m_material};
    m_renderer->castShadows = castShadows;
}

void PrimitiveRenderer::onDestroy()
{
    if (m_renderer && !gameObject().isDestroyed()) gameObject().removeComponent(m_renderer);
    m_renderer = nullptr;
}

} // namespace ve
