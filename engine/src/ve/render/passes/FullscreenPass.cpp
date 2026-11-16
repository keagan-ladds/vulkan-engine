#include "ve/render/passes/FullscreenPass.hpp"

#include "ve/render/Renderer.hpp"

namespace ve {

FullscreenPass::FullscreenPass(std::string name, RenderStage stage, int order, Desc desc)
    : RenderPass(std::move(name), stage, order), m_desc(std::move(desc))
{
}

FullscreenPass::~FullscreenPass()
{
    if (m_pipeline) renderer().destroyPipeline(m_pipeline);
}

void FullscreenPass::setup(RenderGraph& g)
{
    m_output = g.texture(m_desc.output);
    if (m_output == kNoTexture) throw std::runtime_error(name() + ": unknown output texture " + m_desc.output);
    for (const auto& in : m_desc.inputs) {
        const TextureId id = g.texture(in);
        if (id == kNoTexture) throw std::runtime_error(name() + ": unknown input texture " + in);
        m_inputs.push_back(id);
    }
    if (!m_desc.depth.empty()) m_depth = g.texture(m_desc.depth);

    Renderer::FullscreenPipelineDesc p;
    p.fragment = m_desc.fragment;
    // The swapchain image is imported each frame, so take its format from the swapchain.
    p.colorFormats = {m_output == renderer().backbuffer() ? renderer().swapchain().format() : g.get(m_output).desc.format};
    p.blend = m_desc.blend;
    if (m_depth != kNoTexture) {
        p.depthFormat = g.get(m_depth).desc.format;
        p.depthCompare = m_desc.depthCompare;
    }
    m_pipeline = renderer().createFullscreenPipeline(p);
}

void FullscreenPass::declare(PassBuilder& b)
{
    for (TextureId in : m_inputs) b.use(in, Access::Sampled);
    if (m_depth != kNoTexture) b.use(m_depth, Access::DepthRead);
    if (m_desc.samplesShadows) {
        b.use(renderer().dirShadowMap(), Access::Sampled);
        b.use(renderer().pointShadowMap(), Access::Sampled);
    }
    b.use(m_output, Access::ColorAttachment);
}

void FullscreenPass::execute(RenderContext& ctx)
{
    DepthTarget depth;
    if (m_depth != kNoTexture) {
        depth.texture = m_depth;
        depth.readOnly = true;
    }
    ctx.beginRendering({{m_output, m_desc.clear, m_desc.clearColor}}, depth);
    ctx.bindPipeline(m_pipeline);
    if (m_pushSize > 0) ctx.pushConstants(m_push.data(), m_pushSize);
    ctx.drawFullscreen();
    ctx.endRendering();
}

uint32_t FullscreenPass::inputIndex(size_t i) const
{
    return graph().sampled(m_inputs[i]);
}

} // namespace ve
