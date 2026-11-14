#include "vfx/ScreenSpaceGI.hpp"

#include <ve/render/Renderer.hpp>

#include <imgui.h>

namespace vfx {

namespace {
struct TracePush {
    glm::vec4 params;
    uint32_t depthTex;
    uint32_t normalTex;
    uint32_t colorTex;
    uint32_t rays;
};
} // namespace

ScreenSpaceGIPass::ScreenSpaceGIPass(float resolutionScale)
    : RenderPass("Screen-space GI", ve::RenderStage::AfterOpaque, 0), m_scale(resolutionScale)
{
}

ScreenSpaceGIPass::~ScreenSpaceGIPass()
{
    renderer().destroyPipeline(m_trace);
    renderer().destroyPipeline(m_apply);
}

void ScreenSpaceGIPass::setup(ve::RenderGraph& g)
{
    m_buffer = g.createTexture("SSGIBuffer", {.format = VK_FORMAT_R16G16B16A16_SFLOAT, .scale = m_scale});
    ve::Renderer& r = renderer();
    m_trace = r.createFullscreenPipeline({.fragment = "ssgi.frag", .colorFormats = {VK_FORMAT_R16G16B16A16_SFLOAT}});
    // Same upsample + 4x4 box as the fog: dst = src.rgb + dst * src.a.
    m_apply = r.createFullscreenPipeline({.fragment = "fog_apply.frag",
                                          .colorFormats = {ve::Renderer::kSceneColorFormat},
                                          .blend = ve::BlendMode::InscatterTransmittance});
}

void ScreenSpaceGIPass::declare(ve::PassBuilder& b)
{
    ve::Renderer& r = renderer();
    b.use(r.sceneDepth(), ve::Access::Sampled);
    b.use(r.sceneNormal(), ve::Access::Sampled);
    b.use(r.sceneColor(), ve::Access::Sampled); // becomes a colour attachment for the composite
    b.use(m_buffer, ve::Access::ColorAttachment);
}

void ScreenSpaceGIPass::execute(ve::RenderContext& ctx)
{
    ve::Renderer& r = renderer();
    ve::RenderGraph& g = graph();

    TracePush p{};
    p.params = {radius, intensity, aoStrength, static_cast<float>(steps)};
    p.depthTex = g.get(r.sceneDepth()).sampledIndex;
    p.normalTex = g.get(r.sceneNormal()).sampledIndex;
    p.colorTex = g.get(r.sceneColor()).sampledIndex;
    p.rays = static_cast<uint32_t>(rays);

    ctx.beginRendering({{m_buffer}});
    ctx.bindPipeline(m_trace);
    ctx.push(p);
    ctx.drawFullscreen();
    ctx.endRendering();
    g.transition(ctx.cmd, m_buffer, ve::Access::Sampled);

    g.transition(ctx.cmd, r.sceneColor(), ve::Access::ColorAttachment);
    ctx.beginRendering({{r.sceneColor()}});
    ctx.bindPipeline(m_apply);
    ctx.push(g.get(m_buffer).sampledIndex);
    ctx.drawFullscreen();
    ctx.endRendering();
}

void ScreenSpaceGIPass::onGui()
{
    ImGui::SliderFloat("Bounce intensity", &intensity, 0.0f, 4.0f);
    ImGui::SliderFloat("AO strength", &aoStrength, 0.0f, 1.0f);
    ImGui::SliderFloat("Radius", &radius, 0.25f, 10.0f);
    ImGui::SliderInt("Rays", &rays, 1, 16);
    ImGui::SliderInt("Steps", &steps, 2, 32);
}

} // namespace vfx
