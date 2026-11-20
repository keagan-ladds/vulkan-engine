#include "vfx/Water.hpp"

#include <ve/render/Material.hpp>
#include <ve/render/Mesh.hpp>
#include <ve/render/Primitives.hpp>
#include <ve/render/Renderer.hpp>
#include <ve/scene/Components.hpp>

namespace vfx {

void Water::start()
{
    rebuild();
}

void Water::rebuild()
{
    ve::Renderer& r = renderer();
    if (!m_material) {
        ve::ShaderDesc desc;
        desc.name = "Water";
        desc.vertex = "water.vert";
        desc.fragment = "water.frag";
        desc.blend = ve::BlendMode::AlphaBlend;
        desc.queue = ve::RenderQueue::Transparent;
        desc.depthWrite = false;
        desc.castShadows = false;
        desc.cull = ve::CullMode::None;
        m_material = r.createMaterial(r.createShader(desc));
        // The atmosphere's sky LUT is a render-graph texture; it's in the bindless
        // table like any other, so the material can just reference its index.
        const ve::TextureId lut = r.graph().texture("SkyViewLUT");
        m_material->setTextureIndex(0, lut != ve::kNoTexture ? r.graph().sampled(lut) : ve::kInvalidIndex);
    }
    m_material->setVector(0, deepColor);
    m_material->setVector(1, shallowColor);
    m_material->setVector(2, {absorption, shoreFoam, rippleScale, rippleStrength});

    const glm::vec2 s = glm::max(size, glm::vec2(1.0f));
    const auto segments = static_cast<uint32_t>(glm::clamp(s.x / 80.0f, 1.0f, 32.0f));
    auto mesh = ve::Mesh::create(r, ve::primitives::plane(s.x, s.y, segments, segments), {}, "water");
    if (ve::MeshRenderer* mr = m_renderer.get(scene())) {
        mr->mesh = mesh;
    } else {
        auto& added = addComponent<ve::MeshRenderer>(mesh, m_material);
        added.castShadows = false;
        m_renderer = &added;
    }
}

} // namespace vfx
