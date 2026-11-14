#include "vfx/DepthOfField.hpp"

#include <ve/render/Renderer.hpp>

#include <imgui.h>

namespace vfx {

namespace {
struct DofPush { // dof.glsl
    glm::vec4 params;
    glm::vec4 params2;
    uint32_t depthTex;
    uint32_t colorTex;
    uint32_t blurTex;
};
} // namespace

DepthOfFieldPass::DepthOfFieldPass(float resolutionScale)
    : RenderPass("Depth of field", ve::RenderStage::PostProcess, -10), m_scale(resolutionScale)
{
}

DepthOfFieldPass::~DepthOfFieldPass()
{
    renderer().destroyPipeline(m_prepPipeline);
    renderer().destroyPipeline(m_gather);
    renderer().destroyPipeline(m_apply);
}

void DepthOfFieldPass::setup(ve::RenderGraph& g)
{
    m_prep = g.createTexture("DOFPrep", {.format = VK_FORMAT_R16G16B16A16_SFLOAT, .scale = m_scale});
    m_buffer = g.createTexture("DOFBuffer", {.format = VK_FORMAT_R16G16B16A16_SFLOAT, .scale = m_scale});
    ve::Renderer& r = renderer();
    m_prepPipeline = r.createFullscreenPipeline({.fragment = "dof_prep.frag", .colorFormats = {VK_FORMAT_R16G16B16A16_SFLOAT}});
    m_gather = r.createFullscreenPipeline({.fragment = "dof_gather.frag", .colorFormats = {VK_FORMAT_R16G16B16A16_SFLOAT}});
    m_apply = r.createFullscreenPipeline({.fragment = "dof_apply.frag",
                                          .colorFormats = {ve::Renderer::kSceneColorFormat},
                                          .blend = ve::BlendMode::AlphaBlend});
}

void DepthOfFieldPass::declare(ve::PassBuilder& b)
{
    ve::Renderer& r = renderer();
    b.use(r.sceneDepth(), ve::Access::Sampled);
    b.use(r.sceneColor(), ve::Access::Sampled); // becomes a colour attachment for the composite
    b.use(m_prep, ve::Access::ColorAttachment);
    b.use(m_buffer, ve::Access::ColorAttachment);
}

void DepthOfFieldPass::execute(ve::RenderContext& ctx)
{
    ve::Renderer& r = renderer();
    ve::RenderGraph& g = graph();

    DofPush p{};
    p.params = {focusDistance, focusRange, maxBlur, autoFocus ? 1.0f : 0.0f};
    p.params2 = {static_cast<float>(taps), nearBlur, aperture, 0.0f};
    p.depthTex = g.get(r.sceneDepth()).sampledIndex;
    p.colorTex = g.get(r.sceneColor()).sampledIndex;

    // 1. Colour + linear depth at reduced resolution.
    ctx.beginRendering({{m_prep}});
    ctx.bindPipeline(m_prepPipeline);
    ctx.push(p);
    ctx.drawFullscreen();
    ctx.endRendering();
    g.transition(ctx.cmd, m_prep, ve::Access::Sampled);

    // 2. Gather.
    p.blurTex = g.get(m_prep).sampledIndex;
    ctx.beginRendering({{m_buffer}});
    ctx.bindPipeline(m_gather);
    ctx.push(p);
    ctx.drawFullscreen();
    ctx.endRendering();
    g.transition(ctx.cmd, m_buffer, ve::Access::Sampled);

    // 3. Blend over the scene by full-resolution CoC.
    p.blurTex = g.get(m_buffer).sampledIndex;
    g.transition(ctx.cmd, r.sceneColor(), ve::Access::ColorAttachment);
    ctx.beginRendering({{r.sceneColor()}});
    ctx.bindPipeline(m_apply);
    ctx.push(p);
    ctx.drawFullscreen();
    ctx.endRendering();
}

void DepthOfFieldPass::onGui()
{
    ImGui::Checkbox("Auto focus", &autoFocus);
    if (!autoFocus) ImGui::SliderFloat("Focus distance", &focusDistance, 0.2f, 500.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Focus range", &focusRange, 0.0f, 50.0f);
    ImGui::SliderFloat("Aperture", &aperture, 0.1f, 50.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Near blur", &nearBlur, 0.0f, 4.0f);
    ImGui::SliderFloat("Max blur (px)", &maxBlur, 1.0f, 32.0f);
    ImGui::SliderInt("Taps", &taps, 8, 96);
}

} // namespace vfx
