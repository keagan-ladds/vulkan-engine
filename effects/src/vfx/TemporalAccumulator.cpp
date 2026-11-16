#include "vfx/TemporalAccumulator.hpp"

#include <ve/render/Renderer.hpp>

namespace vfx {

namespace {
struct Push { // temporal_accumulate.frag
    float blend;
    float clipGamma;
    uint32_t historyValid;
    uint32_t current;
    uint32_t history;
    uint32_t historyDepth;
    uint32_t depthTex;
};
} // namespace

TemporalAccumulator::~TemporalAccumulator()
{
    if (m_renderer) m_renderer->destroyPipeline(m_pipeline);
}

void TemporalAccumulator::setup(ve::Renderer& renderer, ve::RenderGraph& g, const std::string& name, float scale)
{
    m_renderer = &renderer;
    for (int i = 0; i < 2; ++i) {
        m_history[i] = g.createTexture(name + "History" + std::to_string(i),
                                       {.format = VK_FORMAT_R16G16B16A16_SFLOAT, .scale = scale});
        m_depth[i] = g.createTexture(name + "HistoryDepth" + std::to_string(i),
                                     {.format = VK_FORMAT_R32_SFLOAT, .scale = scale, .sampler = ve::SamplerType::NearestClamp});
    }
    m_pipeline = m_renderer->createFullscreenPipeline(
        {.fragment = "temporal_accumulate.frag", .colorFormats = {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32_SFLOAT}});
    m_valid = false;
}

void TemporalAccumulator::declare(ve::PassBuilder& b)
{
    if (!enabled) return;
    const uint32_t w = m_frame & 1u;
    b.use(m_history[w], ve::Access::ColorAttachment);
    b.use(m_depth[w], ve::Access::ColorAttachment);
    b.use(m_history[w ^ 1u], ve::Access::Sampled);
    b.use(m_depth[w ^ 1u], ve::Access::Sampled);
}

ve::TextureId TemporalAccumulator::resolve(ve::RenderContext& ctx, ve::TextureId current)
{
    if (!enabled) {
        m_valid = false;
        return current;
    }
    ve::RenderGraph& g = *ctx.graph;
    const uint32_t w = m_frame & 1u;
    Push p{};
    p.blend = blend;
    p.clipGamma = clipGamma;
    p.historyValid = m_valid ? 1u : 0u;
    p.current = g.sampled(current);
    p.history = g.sampled(m_history[w ^ 1u]);
    p.historyDepth = g.sampled(m_depth[w ^ 1u]);
    p.depthTex = g.sampled(m_renderer->sceneDepth());
    ctx.fullscreen({{m_history[w]}, {m_depth[w]}}, m_pipeline, p);
    // Both become next frame's (sampled) history.
    g.transition(ctx.cmd, m_history[w], ve::Access::Sampled);
    g.transition(ctx.cmd, m_depth[w], ve::Access::Sampled);
    m_valid = true;
    ++m_frame;
    return m_history[w];
}

} // namespace vfx
