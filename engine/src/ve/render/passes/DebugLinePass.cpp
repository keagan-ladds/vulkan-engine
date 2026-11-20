#include "ve/render/passes/DebugLinePass.hpp"

#include "ve/debug/Debug.hpp"
#include "ve/gfx/Pipeline.hpp"
#include "ve/render/Renderer.hpp"

#include <imgui.h>

#include <cstring>

namespace ve {

namespace {
struct LinePush { // ve.glsl's draw block: model[0][0] = alpha (debug_line.vert)
    glm::mat4 model;
};
LinePush alpha(float a)
{
    LinePush p{glm::mat4(0.0f)};
    p.model[0][0] = a;
    return p;
}
} // namespace

DebugLinePass::~DebugLinePass()
{
    Renderer& r = renderer();
    r.destroyPipeline(m_visible);
    r.destroyPipeline(m_hidden);
    r.destroyPipeline(m_onTop);
    Device* device = &r.device();
    for (Buffer b : m_buffers)
        if (b.handle) device->deferDestroy([device, b]() mutable { device->destroyBuffer(b); });
}

void DebugLinePass::setup(RenderGraph&)
{
    Renderer& r = renderer();
    for (auto& b : m_buffers)
        b = r.device().createBuffer(sizeof(debug::Line) * kMaxLines, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, MemoryUsage::CpuToGpu,
                                    "debug lines");
    const VkVertexInputBindingDescription binding{0, sizeof(debug::Line), VK_VERTEX_INPUT_RATE_INSTANCE};
    const VkVertexInputAttributeDescription attrs[3] = {
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(debug::Line, a)},
        {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(debug::Line, b)},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(debug::Line, color)},
    };
    auto build = [&](bool test, VkCompareOp op) {
        VkShaderModule vs = r.loadShader("debug_line.vert");
        VkShaderModule fs = r.loadShader("debug_line.frag");
        VkPipeline p = PipelineBuilder()
                           .shaders(vs, fs)
                           .vertexInput({&binding, 1}, attrs)
                           .colorAttachment(r.swapchain().format(), BlendMode::AlphaBlend)
                           .depthAttachment(Renderer::kDepthFormat)
                           .depthTest(test, false, op)
                           .layout(r.pipelineLayout())
                           .build(r.device().handle());
        vkDestroyShaderModule(r.device().handle(), vs, nullptr);
        vkDestroyShaderModule(r.device().handle(), fs, nullptr);
        return p;
    };
    // Reversed Z: nearer = greater.
    m_visible = build(true, VK_COMPARE_OP_GREATER_OR_EQUAL);
    m_hidden = build(true, VK_COMPARE_OP_LESS);
    m_onTop = build(false, VK_COMPARE_OP_ALWAYS);
}

void DebugLinePass::prepare(FrameData&)
{
    m_tested = m_untested = 0;
    const auto& lines = debug::lines();
    if (lines.empty()) return;
    m_slot = uint32_t(renderer().frameNumber() % Device::kFramesInFlight); // the renderer's frame slot
    auto* out = static_cast<debug::Line*>(m_buffers[m_slot].mapped);
    // Depth-tested lines first, then the ones on top.
    for (const debug::Line& l : lines)
        if (l.b.w > 0.5f && m_tested < kMaxLines) out[m_tested++] = l;
    for (const debug::Line& l : lines)
        if (l.b.w <= 0.5f && m_tested + m_untested < kMaxLines) out[m_tested + m_untested++] = l;
}

void DebugLinePass::declare(PassBuilder& b)
{
    Renderer& r = renderer();
    b.use(r.backbuffer(), Access::ColorAttachment);
    b.use(r.sceneDepth(), Access::DepthRead);
}

void DebugLinePass::execute(RenderContext& ctx)
{
    if (m_tested + m_untested == 0) return;
    Renderer& r = renderer();
    DepthTarget depth;
    depth.texture = r.sceneDepth();
    depth.readOnly = true;
    ctx.beginRendering({{r.backbuffer()}}, depth);
    VkBuffer vb = m_buffers[m_slot].handle;
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(ctx.cmd, 0, 1, &vb, &offset);
    if (m_tested > 0) {
        ctx.bindPipeline(m_hidden);
        ctx.push(alpha(0.25f));
        vkCmdDraw(ctx.cmd, 6, m_tested, 0, 0);
        ctx.bindPipeline(m_visible);
        ctx.push(alpha(1.0f));
        vkCmdDraw(ctx.cmd, 6, m_tested, 0, 0);
    }
    if (m_untested > 0) {
        ctx.bindPipeline(m_onTop);
        ctx.push(alpha(1.0f));
        vkCmdDraw(ctx.cmd, 6, m_untested, 0, m_tested);
    }
    ctx.endRendering();
}

void DebugLinePass::onGui()
{
    ImGui::Text("Lines: %u depth-tested, %u on top", m_tested, m_untested);
}

} // namespace ve
