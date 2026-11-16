#include "vfx/ScreenSpaceTrace.hpp"

namespace vfx {

ScreenSpaceTracePass::ScreenSpaceTracePass(std::string name, int order, float resolutionScale, Shaders shaders)
    : RenderPass(std::move(name), ve::RenderStage::AfterOpaque, order), m_scale(resolutionScale),
      m_shaders(std::move(shaders))
{
}

ScreenSpaceTracePass::~ScreenSpaceTracePass()
{
    renderer().destroyPipeline(m_trace);
    renderer().destroyPipeline(m_apply);
}

void ScreenSpaceTracePass::setup(ve::RenderGraph& g)
{
    m_buffer = g.createTexture(m_shaders.prefix + "Buffer", {.format = VK_FORMAT_R16G16B16A16_SFLOAT, .scale = m_scale});
    temporal.setup(renderer(), g, m_shaders.prefix, m_scale);
    ve::Renderer& r = renderer();
    m_trace = r.createFullscreenPipeline({.fragment = m_shaders.trace, .colorFormats = {VK_FORMAT_R16G16B16A16_SFLOAT}});
    m_apply = r.createFullscreenPipeline({.fragment = m_shaders.apply,
                                          .colorFormats = {ve::Renderer::kSceneColorFormat},
                                          .blend = m_shaders.applyBlend});
}

void ScreenSpaceTracePass::declare(ve::PassBuilder& b)
{
    ve::Renderer& r = renderer();
    b.use(r.sceneDepth(), ve::Access::Sampled);
    b.use(r.sceneNormal(), ve::Access::Sampled);
    b.use(r.sceneColor(), ve::Access::Sampled); // becomes a colour attachment for the apply
    b.use(m_buffer, ve::Access::ColorAttachment);
    temporal.declare(b);
}

void ScreenSpaceTracePass::execute(ve::RenderContext& ctx)
{
    ve::Renderer& r = renderer();
    ve::RenderGraph& g = graph();
    const Inputs in{g.sampled(r.sceneDepth()), g.sampled(r.sceneNormal()),
                    g.sampled(r.sceneColor())};

    // 1. Trace (the shaders write every pixel, so no clear).
    ctx.beginRendering({{m_buffer}});
    ctx.bindPipeline(m_trace);
    pushTrace(ctx, in);
    ctx.drawFullscreen();
    ctx.endRendering();
    g.transition(ctx.cmd, m_buffer, ve::Access::Sampled);

    // 2. Accumulate over frames (the traces jitter every frame).
    const ve::TextureId result = temporal.resolve(ctx, m_buffer);

    // 3. Upsample and blend over the scene.
    g.transition(ctx.cmd, r.sceneColor(), ve::Access::ColorAttachment);
    ctx.beginRendering({{r.sceneColor()}});
    ctx.bindPipeline(m_apply);
    const VkExtent2D e = g.get(m_buffer).extent;
    pushApply(ctx, in, g.sampled(result), {float(e.width), float(e.height)});
    ctx.drawFullscreen();
    ctx.endRendering();
}

} // namespace vfx
