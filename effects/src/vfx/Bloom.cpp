#include "vfx/Bloom.hpp"

#include <ve/render/Renderer.hpp>

#include <imgui.h>

#include <string>

namespace vfx {

namespace {
struct DownPush {
    glm::vec2 texelSize;
    uint32_t source;
    uint32_t firstPass;
    float threshold;
    float knee;
};
struct UpPush {
    glm::vec2 texelSize;
    uint32_t source;
    float weight;
    float radius;
};
} // namespace

BloomPass::BloomPass(int levels) : RenderPass("Bloom", ve::RenderStage::PostProcess), m_levels(levels) {}

BloomPass::~BloomPass()
{
    renderer().destroyPipeline(m_down);
    renderer().destroyPipeline(m_up);
    renderer().destroyPipeline(m_composite);
}

void BloomPass::setup(ve::RenderGraph& g)
{
    constexpr VkFormat fmt = VK_FORMAT_R16G16B16A16_SFLOAT;
    float scale = 0.5f;
    for (int i = 0; i < m_levels; ++i) {
        m_chain.push_back(g.createTexture("Bloom" + std::to_string(i), {.format = fmt, .scale = scale}));
        scale *= 0.5f;
    }
    ve::Renderer& r = renderer();
    m_down = r.createFullscreenPipeline({.fragment = "bloom_down.frag", .colorFormats = {fmt}});
    m_up = r.createFullscreenPipeline({.fragment = "bloom_up.frag", .colorFormats = {fmt}, .blend = ve::BlendMode::Additive});
    m_composite = r.createFullscreenPipeline(
        {.fragment = "bloom_up.frag", .colorFormats = {ve::Renderer::kSceneColorFormat}, .blend = ve::BlendMode::Additive});
}

void BloomPass::declare(ve::PassBuilder& b)
{
    b.use(renderer().sceneColor(), ve::Access::Sampled);
    for (ve::TextureId t : m_chain) b.use(t, ve::Access::ColorAttachment);
}

void BloomPass::execute(ve::RenderContext& ctx)
{
    ve::RenderGraph& g = graph();
    auto texel = [&](ve::TextureId id) {
        const VkExtent2D e = g.get(id).extent;
        return glm::vec2(1.0f / static_cast<float>(e.width), 1.0f / static_cast<float>(e.height));
    };

    // Downsample chain: SceneColor -> Bloom0 -> Bloom1 -> ...
    ve::TextureId src = renderer().sceneColor();
    for (int i = 0; i < m_levels; ++i) {
        const ve::TextureId dst = m_chain[i];
        ctx.fullscreen({{dst}}, m_down, DownPush{texel(src), g.sampled(src), i == 0 ? 1u : 0u, threshold, knee});
        g.transition(ctx.cmd, dst, ve::Access::Sampled);
        src = dst;
    }
    // Upsample: add each level onto the next larger one.
    for (int i = m_levels - 1; i > 0; --i) {
        const ve::TextureId dst = m_chain[i - 1];
        g.transition(ctx.cmd, dst, ve::Access::ColorAttachment);
        ctx.fullscreen({{dst}}, m_up, UpPush{texel(m_chain[i]), g.sampled(m_chain[i]), 1.0f, radius});
        g.transition(ctx.cmd, dst, ve::Access::Sampled);
    }
    // Composite onto the scene.
    g.transition(ctx.cmd, renderer().sceneColor(), ve::Access::ColorAttachment);
    ctx.fullscreen({{renderer().sceneColor()}}, m_composite, UpPush{texel(m_chain[0]), g.sampled(m_chain[0]), intensity, radius});
}

void BloomPass::onGui()
{
    ImGui::SliderFloat("Intensity", &intensity, 0.0f, 0.5f);
    ImGui::SliderFloat("Threshold", &threshold, 0.0f, 5.0f);
    ImGui::SliderFloat("Knee", &knee, 0.0f, 2.0f);
    ImGui::SliderFloat("Radius", &radius, 0.5f, 3.0f);
}

} // namespace vfx
