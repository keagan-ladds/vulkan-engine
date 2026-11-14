#include "vfx/ScreenSpaceReflections.hpp"

#include <ve/render/Renderer.hpp>

#include <imgui.h>

namespace vfx {

namespace {
struct TracePush {
    glm::vec4 params;
    glm::vec4 params2;
    uint32_t depthTex;
    uint32_t normalTex;
    uint32_t colorTex;
};
struct ApplyPush {
    glm::vec2 texel;
    uint32_t ssr;
};
} // namespace

ScreenSpaceReflectionsPass::ScreenSpaceReflectionsPass(float resolutionScale)
    : RenderPass("Screen-space reflections", ve::RenderStage::AfterOpaque, 10), m_scale(resolutionScale)
{
}

ScreenSpaceReflectionsPass::~ScreenSpaceReflectionsPass()
{
    renderer().destroyPipeline(m_trace);
    renderer().destroyPipeline(m_apply);
}

void ScreenSpaceReflectionsPass::setup(ve::RenderGraph& g)
{
    m_buffer = g.createTexture("SSRBuffer", {.format = VK_FORMAT_R16G16B16A16_SFLOAT, .scale = m_scale});
    ve::Renderer& r = renderer();
    m_trace = r.createFullscreenPipeline({.fragment = "ssr.frag", .colorFormats = {VK_FORMAT_R16G16B16A16_SFLOAT}});
    m_apply = r.createFullscreenPipeline({.fragment = "ssr_apply.frag",
                                          .colorFormats = {ve::Renderer::kSceneColorFormat},
                                          .blend = ve::BlendMode::Premultiplied});
}

void ScreenSpaceReflectionsPass::declare(ve::PassBuilder& b)
{
    ve::Renderer& r = renderer();
    b.use(r.sceneDepth(), ve::Access::Sampled);
    b.use(r.sceneNormal(), ve::Access::Sampled);
    b.use(r.sceneColor(), ve::Access::Sampled); // becomes a colour attachment for the composite
    b.use(m_buffer, ve::Access::ColorAttachment);
}

void ScreenSpaceReflectionsPass::execute(ve::RenderContext& ctx)
{
    ve::Renderer& r = renderer();
    ve::RenderGraph& g = graph();

    TracePush p{};
    p.params = {maxDistance, thickness, static_cast<float>(steps), maxRoughness};
    p.params2 = {intensity, edgeFade, 0.0f, 0.0f};
    p.depthTex = g.get(r.sceneDepth()).sampledIndex;
    p.normalTex = g.get(r.sceneNormal()).sampledIndex;
    p.colorTex = g.get(r.sceneColor()).sampledIndex;

    ctx.beginRendering({{m_buffer, true, glm::vec4(0.0f)}});
    ctx.bindPipeline(m_trace);
    ctx.push(p);
    ctx.drawFullscreen();
    ctx.endRendering();
    g.transition(ctx.cmd, m_buffer, ve::Access::Sampled);

    const VkExtent2D e = g.get(m_buffer).extent;
    g.transition(ctx.cmd, r.sceneColor(), ve::Access::ColorAttachment);
    ctx.beginRendering({{r.sceneColor()}});
    ctx.bindPipeline(m_apply);
    ctx.push(ApplyPush{{1.0f / static_cast<float>(e.width), 1.0f / static_cast<float>(e.height)},
                       g.get(m_buffer).sampledIndex});
    ctx.drawFullscreen();
    ctx.endRendering();
}

void ScreenSpaceReflectionsPass::onGui()
{
    ImGui::SliderFloat("Intensity", &intensity, 0.0f, 2.0f);
    ImGui::SliderFloat("Max distance", &maxDistance, 2.0f, 200.0f);
    ImGui::SliderFloat("Thickness", &thickness, 0.05f, 5.0f);
    ImGui::SliderInt("Steps", &steps, 8, 128);
    ImGui::SliderFloat("Max roughness", &maxRoughness, 0.05f, 1.0f);
    ImGui::SliderFloat("Edge fade", &edgeFade, 0.0f, 0.5f);
}

} // namespace vfx
