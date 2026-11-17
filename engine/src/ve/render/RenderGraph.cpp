#include "ve/render/RenderGraph.hpp"

#include "ve/core/Log.hpp"
#include "ve/gfx/Bindless.hpp"
#include "ve/gfx/Device.hpp"
#include "ve/render/Renderer.hpp"

#include <algorithm>
#include <cmath>

namespace ve {

namespace {

constexpr VkAccessFlags2 kWriteMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT |
                                      VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;

constexpr VkPipelineStageFlags2 kShaderStages = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                                                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

constexpr VkPipelineStageFlags2 kDepthStages = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                               VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;

// The layout/stage/access an image must be in for a given kind of use.
ResourceState stateFor(Access a)
{
    switch (a) {
    case Access::ColorAttachment:
        return {VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT};
    case Access::DepthAttachment:
        return {VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, kDepthStages,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
    case Access::DepthRead:
        return {VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL, kDepthStages, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT};
    case Access::Sampled:
        return {VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL, kShaderStages, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
    case Access::StorageRead:
        return {VK_IMAGE_LAYOUT_GENERAL, kShaderStages, VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
    case Access::StorageWrite:
        return {VK_IMAGE_LAYOUT_GENERAL, kShaderStages, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
    case Access::StorageReadWrite:
        return {VK_IMAGE_LAYOUT_GENERAL, kShaderStages,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
    case Access::TransferSrc:
        return {VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT};
    case Access::TransferDst:
        return {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT};
    case Access::Present:
        return {VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE};
    }
    return {};
}

// Decides whether moving from `current` to the state needed for `access`
// requires a barrier; if so appends one and updates `current`.
void appendTransition(GraphTexture& tex, Access access, std::vector<VkImageMemoryBarrier2>& out)
{
    const ResourceState next = stateFor(access);
    ResourceState& cur = tex.state;
    const bool layoutChange = cur.layout != next.layout;
    const bool hazard = (cur.access & kWriteMask) || (next.access & kWriteMask);
    if (!layoutChange && !hazard) {
        // Read after read in the same layout: no barrier, but remember all
        // readers so a later write waits for every one of them.
        cur.stage |= next.stage;
        cur.access |= next.access;
        return;
    }
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = cur.stage;
    b.srcAccessMask = cur.access;
    b.dstStageMask = next.stage;
    b.dstAccessMask = next.access;
    b.oldLayout = cur.layout;
    b.newLayout = next.layout;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = tex.image.handle;
    b.subresourceRange = {tex.image.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    out.push_back(b);
    cur = next;
}

void flushBarriers(VkCommandBuffer cmd, std::vector<VkImageMemoryBarrier2>& barriers)
{
    if (barriers.empty()) return;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
    dep.pImageMemoryBarriers = barriers.data();
    vkCmdPipelineBarrier2(cmd, &dep);
    barriers.clear();
}

} // namespace

// -----------------------------------------------------------------------------
// PassBuilder / RenderPass
// -----------------------------------------------------------------------------

void PassBuilder::use(TextureId texture, Access access)
{
    if (texture != kNoTexture) usages.push_back({texture, access});
}

RenderPass::RenderPass(std::string name, RenderStage stage, int order)
    : m_name(std::move(name)), m_stage(stage), m_order(order)
{
}

// -----------------------------------------------------------------------------
// RenderContext
// -----------------------------------------------------------------------------

void RenderContext::beginRendering(std::initializer_list<ColorTarget> colors, const DepthTarget& depth)
{
    VkRenderingAttachmentInfo colorInfos[8]{};
    uint32_t colorCount = 0;
    VkExtent2D extent{0, 0};

    for (const ColorTarget& c : colors) {
        const GraphTexture& t = graph->get(c.texture);
        VkRenderingAttachmentInfo& a = colorInfos[colorCount++];
        a.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        a.imageView = c.layer >= 0 ? t.image.layerViews[c.layer] : t.image.view;
        a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        a.loadOp = c.clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.clearValue.color = {{c.clearValue.r, c.clearValue.g, c.clearValue.b, c.clearValue.a}};
        extent = t.extent;
    }

    VkRenderingAttachmentInfo depthInfo{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    const bool hasDepth = depth.texture != kNoTexture;
    if (hasDepth) {
        const GraphTexture& t = graph->get(depth.texture);
        depthInfo.imageView = depth.layer >= 0 ? t.image.layerViews[depth.layer] : t.image.view;
        depthInfo.imageLayout = depth.readOnly ? VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL
                                               : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthInfo.loadOp = depth.clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        depthInfo.storeOp = depth.readOnly ? VK_ATTACHMENT_STORE_OP_NONE : VK_ATTACHMENT_STORE_OP_STORE;
        depthInfo.clearValue.depthStencil = {depth.clearValue, 0};
        if (colorCount == 0) extent = t.extent;
    }

    VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
    info.renderArea = {{0, 0}, extent};
    info.layerCount = 1;
    info.colorAttachmentCount = colorCount;
    info.pColorAttachments = colorInfos;
    info.pDepthAttachment = hasDepth ? &depthInfo : nullptr;
    vkCmdBeginRendering(cmd, &info);
    setViewport(extent);
}

void RenderContext::endRendering()
{
    vkCmdEndRendering(cmd);
}

void RenderContext::setViewport(VkExtent2D extent)
{
    VkViewport vp{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

void RenderContext::bindPipeline(VkPipeline pipeline, VkPipelineBindPoint point)
{
    vkCmdBindPipeline(cmd, point, pipeline);
}

void RenderContext::pushConstants(const void* data, uint32_t size)
{
    vkCmdPushConstants(cmd, renderer->pipelineLayout(), VK_SHADER_STAGE_ALL, 0, size, data);
}

void RenderContext::drawFullscreen()
{
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

// -----------------------------------------------------------------------------
// RenderGraph
// -----------------------------------------------------------------------------

RenderGraph::RenderGraph(Renderer& renderer) : m_renderer(renderer) {}

RenderGraph::~RenderGraph()
{
    clearPasses();
    for (auto& t : m_textures) destroyImage(t, false);
}

void RenderGraph::clearPasses()
{
    m_passes.clear();
}

VkExtent2D RenderGraph::resolveExtent(const GraphTextureDesc& desc) const
{
    if (desc.width > 0 && desc.height > 0) return {desc.width, desc.height};
    return {std::max(1u, static_cast<uint32_t>(std::lround(m_screenExtent.width * desc.scale))),
            std::max(1u, static_cast<uint32_t>(std::lround(m_screenExtent.height * desc.scale)))};
}

void RenderGraph::createImage(GraphTexture& tex)
{
    Device& device = m_renderer.device();
    const GraphTextureDesc& d = tex.desc;
    const bool depth = vk::isDepthFormat(d.format);
    tex.extent = resolveExtent(d);

    ImageDesc img;
    img.format = d.format;
    img.extent = {tex.extent.width, tex.extent.height, 1};
    img.mipLevels = d.mips;
    img.layers = d.layers;
    img.viewType = d.viewType;
    img.perLayerViews = d.perLayerViews;
    img.debugName = tex.name.c_str();
    img.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
                (d.storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0) | d.extraUsage;
    tex.image = device.createImage(img);
    tex.state = {};

    Bindless& bindless = m_renderer.bindless();
    VkSampler sampler = m_renderer.samplers().get(d.sampler);
    if (tex.sampledIndex == Bindless::kInvalid) tex.sampledIndex = bindless.addTexture(tex.image.view, sampler);
    else bindless.updateTexture(tex.sampledIndex, tex.image.view, sampler);
    if (d.storage) {
        if (tex.storageIndex == Bindless::kInvalid) tex.storageIndex = bindless.addStorageImage(tex.image.view);
        else bindless.updateStorageImage(tex.storageIndex, tex.image.view);
    }
}

void RenderGraph::destroyImage(GraphTexture& tex, bool deferred)
{
    if (tex.imported || !tex.image.handle) return;
    Device* device = &m_renderer.device();
    if (deferred) {
        Image img = tex.image;
        device->deferDestroy([device, img]() mutable { device->destroyImage(img); });
    } else {
        device->destroyImage(tex.image);
    }
    tex.image = Image{};
}

TextureId RenderGraph::createTexture(const std::string& name, const GraphTextureDesc& desc)
{
    if (m_byName.contains(name)) {
        throw std::runtime_error("RenderGraph: texture '" + name + "' already exists");
    }
    GraphTexture tex;
    tex.name = name;
    tex.desc = desc;
    createImage(tex);
    const auto id = static_cast<TextureId>(m_textures.size());
    m_textures.push_back(std::move(tex));
    m_byName[name] = id;
    return id;
}

void RenderGraph::recreateTexture(TextureId id, const GraphTextureDesc& desc)
{
    GraphTexture& tex = m_textures[id];
    destroyImage(tex, true);
    tex.desc = desc;
    createImage(tex);
}

TextureId RenderGraph::importTexture(const std::string& name)
{
    GraphTexture tex;
    tex.name = name;
    tex.imported = true;
    const auto id = static_cast<TextureId>(m_textures.size());
    m_textures.push_back(std::move(tex));
    m_byName[name] = id;
    return id;
}

void RenderGraph::setImported(TextureId id, VkImage image, VkImageView view, VkFormat format, VkExtent2D extent,
                              const ResourceState& state)
{
    GraphTexture& t = m_textures[id];
    t.image.handle = image;
    t.image.view = view;
    t.image.format = format;
    t.image.extent = {extent.width, extent.height, 1};
    t.image.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    t.desc.format = format;
    t.extent = extent;
    t.state = state;
}

TextureId RenderGraph::texture(const std::string& name) const
{
    const auto it = m_byName.find(name);
    return it == m_byName.end() ? kNoTexture : it->second;
}

void RenderGraph::initializeDepth(TextureId id, float clearValue)
{
    GraphTexture& t = m_textures[id];
    m_renderer.device().immediateSubmit([&](VkCommandBuffer cmd) {
        std::vector<VkImageMemoryBarrier2> barriers;
        t.state = {};
        appendTransition(t, Access::TransferDst, barriers);
        flushBarriers(cmd, barriers);
        VkClearDepthStencilValue value{clearValue, 0};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        vkCmdClearDepthStencilImage(cmd, t.image.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
        appendTransition(t, Access::Sampled, barriers);
        flushBarriers(cmd, barriers);
    });
}

void RenderGraph::addPassImpl(std::unique_ptr<RenderPass> pass)
{
    pass->m_renderer = &m_renderer;
    pass->m_graph = this;
    pass->m_sequence = m_sequence++;
    pass->setup(*this);
    m_passes.push_back(std::move(pass));
    std::stable_sort(m_passes.begin(), m_passes.end(), [](const auto& a, const auto& b) {
        if (a->m_stage != b->m_stage) return a->m_stage < b->m_stage;
        if (a->m_order != b->m_order) return a->m_order < b->m_order;
        return a->m_sequence < b->m_sequence;
    });
}

void RenderGraph::removePass(const std::string& name)
{
    std::erase_if(m_passes, [&](const auto& p) { return p->name() == name; });
}

RenderPass* RenderGraph::findPass(const std::string& name) const
{
    for (const auto& p : m_passes) {
        if (p->name() == name) return p.get();
    }
    return nullptr;
}

void RenderGraph::resize(VkExtent2D screenExtent)
{
    m_screenExtent = screenExtent;
    // The renderer waits for the GPU to be idle before calling this, so the
    // old images can be destroyed immediately.
    for (auto& t : m_textures) {
        if (t.imported || (t.desc.width > 0 && t.desc.height > 0)) continue;
        destroyImage(t, false);
        createImage(t);
    }
    for (auto& p : m_passes) p->onResize(*this);
}

void RenderGraph::prepare(FrameData& frame)
{
    for (auto& p : m_passes) {
        if (p->enabled && int(p->stage()) < maxStage && (!loadingFrame || p->runsWhileLoading)) p->prepare(frame);
    }
}

void RenderGraph::execute(RenderContext& ctx)
{
    m_executed.clear();
    for (auto& pass : m_passes) {
        if (!pass->enabled || int(pass->stage()) >= maxStage || (loadingFrame && !pass->runsWhileLoading)) continue;
        PassBuilder builder(*this);
        pass->declare(builder);
        for (const auto& u : builder.usages) {
            appendTransition(m_textures[u.texture], u.access, m_barriers);
        }
        flushBarriers(ctx.cmd, m_barriers);
        m_renderer.bindGlobalSets(ctx.cmd);
        const auto q = static_cast<uint32_t>(m_executed.size() * 2);
        const bool timed = m_queryPool && q + 1 < m_queryCapacity;
        if (timed) vkCmdWriteTimestamp2(ctx.cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m_queryPool, q);
        pass->execute(ctx);
        if (timed) vkCmdWriteTimestamp2(ctx.cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m_queryPool, q + 1);
        m_executed.push_back(pass.get());
    }
}

void RenderGraph::applyTimings(const std::vector<uint64_t>& ts, const std::vector<RenderPass*>& order, float periodNs)
{
    for (size_t i = 0; i < order.size() && i * 2 + 1 < ts.size(); ++i) {
        // Make sure the pass still exists (it may have been removed since).
        for (const auto& p : m_passes) {
            if (p.get() != order[i]) continue;
            const float ms = static_cast<float>(ts[i * 2 + 1] - ts[i * 2]) * periodNs * 1e-6f;
            p->lastGpuTimeMs = ms;
            p->gpuTimeMs = p->gpuTimeMs * 0.9f + ms * 0.1f; // smooth
        }
    }
}

void RenderGraph::transition(VkCommandBuffer cmd, TextureId id, Access access)
{
    appendTransition(m_textures[id], access, m_barriers);
    flushBarriers(cmd, m_barriers);
}

} // namespace ve
