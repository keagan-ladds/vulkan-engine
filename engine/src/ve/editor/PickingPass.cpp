#include "ve/editor/PickingPass.hpp"

#include "ve/gfx/Pipeline.hpp"
#include "ve/render/Mesh.hpp"
#include "ve/render/Renderer.hpp"
#include "ve/scene/GameObject.hpp"

#include <vk_mem_alloc.h>

#include <algorithm>
#include <cmath>

namespace ve {

PickingPass::PickingPass() : RenderPass("Editor picking", RenderStage::Overlay, -100)
{
    enabled = false;
}

PickingPass::~PickingPass()
{
    renderer().destroyPipeline(m_pipeline);
    if (m_readback.handle) renderer().device().destroyBuffer(m_readback);
}

void PickingPass::setup(RenderGraph& g)
{
    m_ids = g.createTexture("PickIds", {.format = VK_FORMAT_R32_SFLOAT,
                                        .extraUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                        .sampler = SamplerType::NearestClamp});
    m_depth = g.createTexture("PickDepth", {.format = Renderer::kDepthFormat, .sampler = SamplerType::NearestClamp});

    Renderer& r = renderer();
    const auto binding = Vertex::binding();
    const auto attributes = Vertex::attributes();
    VkShaderModule vs = r.loadShader("pick.vert");
    VkShaderModule fs = r.loadShader("pick.frag");
    m_pipeline = PipelineBuilder()
                     .shaders(vs, fs)
                     .vertexInput({&binding, 1}, attributes)
                     .cull(CullMode::None)
                     .colorAttachment(VK_FORMAT_R32_SFLOAT)
                     .depthAttachment(Renderer::kDepthFormat)
                     .depthTest(true, true, VK_COMPARE_OP_GREATER_OR_EQUAL) // reversed-Z
                     .layout(r.pipelineLayout())
                     .build(r.device().handle());
    vkDestroyShaderModule(r.device().handle(), vs, nullptr);
    vkDestroyShaderModule(r.device().handle(), fs, nullptr);
    m_readback = r.device().createBuffer(sizeof(float), VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::GpuToCpu,
                                         "pick readback");
}

void PickingPass::request(glm::ivec2 pixel)
{
    if (m_state != State::Idle) return;
    m_pixel = pixel;
    m_state = State::Requested;
    enabled = true;
}

void PickingPass::declare(PassBuilder& b)
{
    b.use(m_ids, Access::ColorAttachment);
    b.use(m_depth, Access::DepthAttachment);
}

void PickingPass::execute(RenderContext& ctx)
{
    Renderer& r = renderer();
    RenderGraph& g = graph();
    const FrameData& frame = *ctx.frame;
    const VkExtent2D e = g.get(m_ids).extent;
    const glm::ivec2 px = glm::clamp(m_pixel, glm::ivec2(0), glm::ivec2(int(e.width) - 1, int(e.height) - 1));

    ctx.beginRendering({{m_ids, true, glm::vec4(0.0f)}}, {m_depth, true, 0.0f});
    const VkRect2D scissor{{px.x, px.y}, {1, 1}};
    vkCmdSetScissor(ctx.cmd, 0, 1, &scissor);
    ctx.bindPipeline(m_pipeline);

    m_owners.clear();
    const Mesh* bound = nullptr;
    auto drawList = [&](const std::vector<uint32_t>& list) {
        for (uint32_t idx : list) {
            const DrawItem& it = frame.items[idx];
            if (!it.owner) continue;
            if (it.mesh != bound) {
                VkBuffer vb = it.mesh->vertexBuffer();
                VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(ctx.cmd, 0, 1, &vb, &offset);
                vkCmdBindIndexBuffer(ctx.cmd, it.mesh->indexBuffer(), 0, VK_INDEX_TYPE_UINT32);
                bound = it.mesh;
            }
            m_owners.push_back(it.owner->id());
            DrawPush push{};
            push.model = it.world;
            push.viewIndex = static_cast<uint32_t>(m_owners.size()); // 1-based
            vkCmdPushConstants(ctx.cmd, r.pipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(DrawPush), &push);
            vkCmdDrawIndexed(ctx.cmd, it.submesh->indexCount, 1, it.submesh->firstIndex, 0, 0);
        }
    };
    drawList(frame.opaque);
    drawList(frame.transparent);
    ctx.endRendering();

    g.transition(ctx.cmd, m_ids, Access::TransferSrc);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageOffset = {px.x, px.y, 0};
    copy.imageExtent = {1, 1, 1};
    vkCmdCopyImageToBuffer(ctx.cmd, g.get(m_ids).image.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_readback.handle, 1,
                           &copy);

    m_renderedFrame = r.frameNumber();
    m_state = State::Rendered;
    enabled = false; // one-shot
}

std::optional<uint64_t> PickingPass::result()
{
    // The CPU runs at most kFramesInFlight frames ahead: after that the copy is done.
    if (m_state != State::Rendered || renderer().frameNumber() < m_renderedFrame + Device::kFramesInFlight + 1)
        return std::nullopt;
    m_state = State::Idle;
    vmaInvalidateAllocation(renderer().device().allocator(), m_readback.allocation, 0, VK_WHOLE_SIZE);
    const float v = *static_cast<const float*>(m_readback.mapped);
    const auto index = static_cast<int64_t>(std::lround(v));
    if (index <= 0 || index > static_cast<int64_t>(m_owners.size())) return uint64_t(0);
    return m_owners[static_cast<size_t>(index - 1)];
}

} // namespace ve
