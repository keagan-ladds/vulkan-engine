#include "ve/render/PipelineCompiler.hpp"

#include "ve/core/Log.hpp"
#include "ve/gfx/Device.hpp"
#include "ve/render/Geometry.hpp"
#include "ve/render/Renderer.hpp"

#include <algorithm>
#include <exception>

namespace ve {

VkPipeline buildMeshPipeline(const MeshPipelineInputs& in)
{
    const auto binding = Vertex::binding();
    const auto attributes = Vertex::attributes();
    const ShaderDesc& desc = in.desc;

    VkShaderModule vert = loadShaderModule(in.device, in.vertex);
    VkShaderModule frag = VK_NULL_HANDLE;
    try {
        if (!in.fragment.empty()) frag = loadShaderModule(in.device, in.fragment);

        PipelineBuilder b;
        b.layout(in.layout).shaders(vert, frag).vertexInput({&binding, 1}, attributes);
        if (in.kind == PassKind::Forward) {
            const bool wire = desc.wireframe && in.fillModeNonSolid;
            // Reversed-Z: closer = larger depth, so the test is GREATER_OR_EQUAL.
            b.cull(desc.cull)
                .polygonMode(wire ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL)
                .colorAttachment(Renderer::kSceneColorFormat, desc.blend)
                // Opaque shaders also write the thin G-buffer; transparent ones leave it alone.
                .colorAttachment(Renderer::kSceneNormalFormat, BlendMode::Opaque, desc.queue < RenderQueue::Transparent)
                .depthAttachment(Renderer::kDepthFormat)
                .depthTest(desc.depthTest, desc.depthWrite, VK_COMPARE_OP_GREATER_OR_EQUAL);
        } else {
            // Shadow maps use standard depth (LESS). No culling: thin geometry
            // (leaves, open meshes) must still cast shadows from both sides.
            b.cull(CullMode::None)
                .depthAttachment(Renderer::kShadowFormat)
                .depthTest(true, true, VK_COMPARE_OP_LESS_OR_EQUAL)
                .dynamicDepthBias(true)
                .depthClamp(in.kind == PassKind::ShadowDirectional && in.depthClamp);
        }
        VkPipeline pipeline = b.build(in.device);
        vkDestroyShaderModule(in.device, vert, nullptr);
        if (frag) vkDestroyShaderModule(in.device, frag, nullptr);
        return pipeline;
    } catch (...) {
        vkDestroyShaderModule(in.device, vert, nullptr);
        if (frag) vkDestroyShaderModule(in.device, frag, nullptr);
        throw;
    }
}

std::shared_ptr<PipelineCompile> PipelineCompiler::submit(MeshPipelineInputs inputs)
{
    auto c = std::make_shared<PipelineCompile>();
    if (m_shutdown) {
        c->error = "renderer shut down";
        return c; // an empty handle is done()
    }
    if (m_inFlight.empty() && m_burstCount == 0) m_burstStart = Clock::now();
    // m_inFlight keeps the record alive until the job is done (also when its
    // Shader dies meanwhile), so the job can point at it without owning it
    // (owning it would be a cycle: record -> handle -> job -> record).
    c->job = jobs::submit([rec = c.get(), in = std::move(inputs)](const std::atomic<bool>&) {
        const auto start = Clock::now();
        try {
            rec->pipeline = buildMeshPipeline(in);
        } catch (const std::exception& e) {
            rec->error = e.what();
        }
        rec->ms = std::chrono::duration<float, std::milli>(Clock::now() - start).count();
    });
    m_inFlight.push_back(c);
    ++m_submitted;
    return c;
}

void PipelineCompiler::poll()
{
    if (m_shutdown) return;
    const auto now = Clock::now();
    for (const auto& c : m_inFlight) {
        if (!c->job.done()) continue;
        ++m_finished;
        ++m_burstCount;
        m_burstSlowestMs = std::max(m_burstSlowestMs, c->ms);
        m_lastFinish = now;
        if (c->pipeline) m_cacheDirty = true;
        if (c->abandoned && !c->claimed && c->pipeline) {
            VkPipeline p = c->pipeline;
            c->pipeline = VK_NULL_HANDLE;
            Device* device = m_device;
            device->deferDestroy([device, p] { vkDestroyPipeline(device->handle(), p, nullptr); });
        }
    }
    std::erase_if(m_inFlight, [](const auto& c) { return c->job.done(); });

    if (m_inFlight.empty() && m_burstCount > 0) {
        const float seconds = std::chrono::duration<float>(m_lastFinish - m_burstStart).count();
        log::info("Pipelines: built {} in {:.2f} s (slowest {:.0f} ms)", m_burstCount, seconds,
                  m_burstSlowestMs);
        m_burstCount = 0;
        m_burstSlowestMs = 0.0f;
    }
    // Save once things are quiet, so a crash or a killed process keeps the work.
    if (m_cacheDirty && m_inFlight.empty() && now - m_lastFinish > std::chrono::seconds(1)) {
        m_device->savePipelineCache();
        m_cacheDirty = false;
    }
}

void PipelineCompiler::shutdown()
{
    if (m_shutdown) return;
    VkDevice dev = m_device ? m_device->handle() : VK_NULL_HANDLE;
    for (const auto& c : m_inFlight) {
        c->job.wait();
        if (!c->claimed && c->pipeline && dev) vkDestroyPipeline(dev, c->pipeline, nullptr);
        c->pipeline = VK_NULL_HANDLE;
        c->claimed = true; // nothing left for a Shader to take
    }
    m_inFlight.clear();
    m_shutdown = true;
}

size_t PipelineCompiler::pending() const
{
    return size_t(std::count_if(m_inFlight.begin(), m_inFlight.end(), [](const auto& c) { return !c->job.done(); }));
}

uint64_t PipelineCompiler::finishedCount() const
{
    return m_finished + (m_inFlight.size() - pending());
}

} // namespace ve
