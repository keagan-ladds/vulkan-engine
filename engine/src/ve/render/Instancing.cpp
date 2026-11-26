#include "ve/render/Instancing.hpp"

#include "ve/core/Log.hpp"
#include "ve/gfx/Device.hpp"
#include "ve/render/Mesh.hpp"
#include "ve/render/Renderer.hpp"

#include <algorithm>
#include <cstring>

namespace ve {

namespace {

GpuInstance pack(const glm::mat4& m, const glm::vec4& custom)
{
    GpuInstance g;
    // Row i of the affine transform = (m[0][i], m[1][i], m[2][i], m[3][i]) in glm's column-major storage.
    for (int i = 0; i < 3; ++i) g.rows[i] = glm::vec4(m[0][i], m[1][i], m[2][i], m[3][i]);
    g.custom = custom;
    return g;
}

glm::mat4 unpack(const GpuInstance& g)
{
    glm::mat4 m(1.0f);
    for (int i = 0; i < 3; ++i) {
        m[0][i] = g.rows[i].x;
        m[1][i] = g.rows[i].y;
        m[2][i] = g.rows[i].z;
        m[3][i] = g.rows[i].w;
    }
    return m;
}

struct CullPush {
    uint32_t groupIndex;
    uint32_t batchCount;
    float lodScale;
    float lodBias;
    uint32_t shadowLodBias;
    float fadeBand;
    uint32_t viewCount;
    uint32_t hizValid;            // camera occlusion test against last frame's depth pyramid
    uint32_t hiz[kHiZLevels];     // pyramid levels (bindless), 512x256 .. 1x1
};

} // namespace

// =============================================================================
// InstanceGroup
// =============================================================================

InstanceGroup::InstanceGroup(InstanceSystem& system, InstanceGroupDesc desc, uint32_t capacity, uint32_t firstInstance)
    : m_system(&system), m_desc(std::move(desc)), m_capacity(capacity), m_firstInstance(firstInstance)
{
    m_data.reserve(capacity);
    // Bounding sphere around all LODs (centred on LOD 0) for culling.
    const AABB& b0 = m_desc.lods[0].mesh->bounds();
    const glm::vec3 c = b0.center();
    float r = 0.0f;
    for (const auto& lod : m_desc.lods) {
        const AABB& b = lod.mesh->bounds();
        r = std::max(r, glm::length(b.center() - c) + glm::length(b.extents()));
    }
    m_localSphere = glm::vec4(c, r);
}

InstanceGroup::~InstanceGroup()
{
    m_system->release(this);
}

void InstanceGroup::markDirty(uint32_t slot)
{
    if (m_dirtyBegin == m_dirtyEnd) {
        m_dirtyBegin = slot;
        m_dirtyEnd = slot + 1;
    } else {
        m_dirtyBegin = std::min(m_dirtyBegin, slot);
        m_dirtyEnd = std::max(m_dirtyEnd, slot + 1);
    }
}

InstanceGroup::Handle InstanceGroup::add(const glm::mat4& transform, const glm::vec4& custom)
{
    if (m_data.size() >= m_capacity) {
        throw std::runtime_error("InstanceGroup '" + m_desc.name + "' is full (capacity " + std::to_string(m_capacity) + ")");
    }
    Handle h;
    if (!m_freeHandles.empty()) {
        h = m_freeHandles.back();
        m_freeHandles.pop_back();
    } else {
        h = static_cast<Handle>(m_handleToSlot.size());
        m_handleToSlot.push_back(kInvalid);
    }
    const auto slot = static_cast<uint32_t>(m_data.size());
    m_data.push_back(pack(transform, custom));
    m_slotToHandle.push_back(h);
    m_handleToSlot[h] = slot;
    markDirty(slot);
    return h;
}

void InstanceGroup::set(Handle h, const glm::mat4& transform)
{
    const uint32_t slot = m_handleToSlot.at(h);
    m_data[slot] = pack(transform, m_data[slot].custom);
    markDirty(slot);
}

void InstanceGroup::setCustom(Handle h, const glm::vec4& custom)
{
    const uint32_t slot = m_handleToSlot.at(h);
    m_data[slot].custom = custom;
    markDirty(slot);
}

void InstanceGroup::remove(Handle h)
{
    if (h >= m_handleToSlot.size() || m_handleToSlot[h] == kInvalid) return;
    const uint32_t slot = m_handleToSlot[h];
    const auto last = static_cast<uint32_t>(m_data.size() - 1);
    // Keep the instances packed: move the last one into the hole.
    if (slot != last) {
        m_data[slot] = m_data[last];
        const Handle moved = m_slotToHandle[last];
        m_slotToHandle[slot] = moved;
        m_handleToSlot[moved] = slot;
        markDirty(slot);
    }
    m_data.pop_back();
    m_slotToHandle.pop_back();
    m_handleToSlot[h] = kInvalid;
    m_freeHandles.push_back(h);
}

void InstanceGroup::clear()
{
    m_data.clear();
    m_slotToHandle.clear();
    m_handleToSlot.clear();
    m_freeHandles.clear();
    m_dirtyBegin = m_dirtyEnd = 0;
}

glm::mat4 InstanceGroup::transform(Handle h) const
{
    return unpack(m_data[m_handleToSlot.at(h)]);
}

// =============================================================================
// InstanceSystem
// =============================================================================

void InstanceSystem::init(Renderer& renderer, uint32_t maxInstances, uint32_t maxVisiblePerView)
{
    m_renderer = &renderer;
    if (maxInstances > (1u << 23)) throw std::runtime_error("maxInstances is limited to 8M (23-bit visible entries)");
    m_maxInstances = maxInstances;
    m_maxVisiblePerView = maxVisiblePerView;
    Device& d = renderer.device();

    m_instances = d.createBuffer(VkDeviceSize(maxInstances) * sizeof(GpuInstance), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                 MemoryUsage::GpuOnly, "instances");
    // The visible list starts small and grows with the groups (ensureVisibleCapacity).
    m_visibleCapacity = 1u << 16;
    m_visible = d.createBuffer(VkDeviceSize(m_visibleCapacity) * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               MemoryUsage::GpuOnly, "visible instances");
    const VkDeviceSize commandBytes = VkDeviceSize(kMaxViews) * kMaxBatches * sizeof(DrawIndexedIndirect);
    m_commands = d.createBuffer(commandBytes,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                MemoryUsage::GpuOnly, "instance draw commands");
    for (Slot& s : m_slots) {
        s.groups = d.createBuffer(kMaxGroups * sizeof(GpuInstanceGroup), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                  MemoryUsage::CpuToGpu, "instance groups");
        s.views = d.createBuffer(VkDeviceSize(kMaxViews) * sizeof(GpuCullView), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                 MemoryUsage::CpuToGpu, "cull views");
        s.commands = d.createBuffer(commandBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUsage::CpuToGpu,
                                    "instance command template");
        s.staging = d.createBuffer(1u << 20, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUsage::CpuToGpu, "instance staging");
        s.readback = d.createBuffer(commandBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::GpuToCpu,
                                    "instance stats readback");
    }
    m_freeRanges.push_back({0, maxInstances});
    m_cullPipeline = renderer.createComputePipeline("instance_cull.comp");
}

void InstanceSystem::shutdown()
{
    if (!m_renderer) return;
    if (!m_groups.empty()) {
        log::warn("InstanceSystem: {} instance group(s) still alive at shutdown", m_groups.size());
    }
    Device& d = m_renderer->device();
    m_renderer->destroyPipeline(m_cullPipeline);
    d.destroyBuffer(m_instances);
    d.destroyBuffer(m_visible);
    d.destroyBuffer(m_commands);
    for (Slot& s : m_slots) {
        d.destroyBuffer(s.groups);
        d.destroyBuffer(s.views);
        d.destroyBuffer(s.commands);
        d.destroyBuffer(s.staging);
        d.destroyBuffer(s.readback);
    }
    m_renderer = nullptr;
}

uint32_t InstanceSystem::allocateRange(uint32_t count)
{
    for (auto it = m_freeRanges.begin(); it != m_freeRanges.end(); ++it) {
        if (it->count < count) continue;
        const uint32_t first = it->first;
        it->first += count;
        it->count -= count;
        if (it->count == 0) m_freeRanges.erase(it);
        return first;
    }
    throw std::runtime_error("Instance buffer full: raise RendererSettings::maxInstances");
}

std::shared_ptr<InstanceGroup> InstanceSystem::createGroup(InstanceGroupDesc desc, uint32_t capacity)
{
    if (desc.lods.empty() || desc.lods.size() > 4) throw std::runtime_error("InstanceGroup needs 1..4 LODs");
    for (const auto& lod : desc.lods) {
        if (!lod.mesh) throw std::runtime_error("InstanceGroup '" + desc.name + "': LOD without a mesh");
    }
    if (m_groups.size() >= kMaxGroups) throw std::runtime_error("Too many instance groups");
    const uint32_t first = allocateRange(std::max(1u, capacity));
    std::shared_ptr<InstanceGroup> group(new InstanceGroup(*this, std::move(desc), capacity, first));
    m_groups.push_back(group.get());
    m_layoutDirty = true;
    return group;
}

void InstanceSystem::release(InstanceGroup* group)
{
    std::erase(m_groups, group);
    m_layoutDirty = true;
    // Batches point at the group: drop them now (not at the next prepare) so a
    // group destroyed mid-frame is never dereferenced by culling or drawing.
    m_batches.clear();
    m_forwardBatches.clear();
    m_shadowBatches.clear();
    if (!m_renderer) return;
    // The GPU may still be culling/drawing this range for in-flight frames.
    const Range range{group->m_firstInstance, std::max(1u, group->m_capacity)};
    m_renderer->device().deferDestroy([this, range] { m_freeRanges.push_back(range); });
}

void InstanceSystem::rebuildLayout()
{
    m_batches.clear();
    uint32_t visibleOffset = 0;
    for (InstanceGroup* g : m_groups) {
        for (uint32_t l = 0; l < g->m_desc.lods.size(); ++l) {
            const InstanceLod& lod = g->m_desc.lods[l];
            for (const Submesh& sm : lod.mesh->submeshes()) {
                Material* mat = nullptr;
                if (!lod.materials.empty()) {
                    mat = lod.materials[std::min<size_t>(sm.materialSlot, lod.materials.size() - 1)].get();
                }
                if (!mat) mat = m_renderer->fallbackMaterial();
                m_batches.push_back({g, l, lod.mesh.get(), sm.firstIndex, sm.indexCount, mat, visibleOffset});
                visibleOffset += g->m_capacity;
            }
        }
    }
    if (m_batches.size() > kMaxBatches) throw std::runtime_error("Too many instance batches (groups x LODs x submeshes)");
    if (visibleOffset > m_maxVisiblePerView) {
        throw std::runtime_error("Instance groups need " + std::to_string(visibleOffset) +
                                 " visible slots per view: raise RendererSettings::maxVisibleInstancesPerView");
    }
    m_visiblePerView = visibleOffset;

    // Per-pass draw lists, sorted by pipeline (shader) then mesh.
    m_forwardBatches.clear();
    m_shadowBatches.clear();
    for (uint32_t b = 0; b < m_batches.size(); ++b) {
        const Batch& bt = m_batches[b];
        const Shader& shader = bt.material->shader();
        if (shader.transparent()) {
            log::warn("Instance group '{}': transparent materials aren't supported for instancing; skipped",
                      bt.group->m_desc.name);
        } else {
            m_forwardBatches.push_back(b);
        }
        if (bt.group->m_desc.castShadows && shader.desc().castShadows) m_shadowBatches.push_back(b);
    }
    auto byState = [&](uint32_t a, uint32_t b) {
        const Batch &x = m_batches[a], &y = m_batches[b];
        const Shader *sx = &x.material->shader(), *sy = &y.material->shader();
        return sx != sy ? sx < sy : x.mesh < y.mesh;
    };
    std::sort(m_forwardBatches.begin(), m_forwardBatches.end(), byState);
    std::sort(m_shadowBatches.begin(), m_shadowBatches.end(), byState);
}

void InstanceSystem::ensureVisibleCapacity(uint32_t entries)
{
    if (entries <= m_visibleCapacity) return;
    // Only happens when groups are added or more shadow views appear (level
    // load, settings change), so simply wait for the GPU and swap the buffer.
    Device& d = m_renderer->device();
    d.waitIdle();
    d.destroyBuffer(m_visible);
    m_visibleCapacity = std::max(entries, m_visibleCapacity + m_visibleCapacity / 2);
    m_visible = d.createBuffer(VkDeviceSize(m_visibleCapacity) * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               MemoryUsage::GpuOnly, "visible instances");
    m_renderer->writeInstanceDescriptors();
    // Reserved room, not a count: every view (camera + shadow cascades) gets a
    // slot per instance the groups *could* hold, per LOD and submesh batch.
    log::info("InstanceSystem: room for {} visible-instance entries ({} views x {} slots: group capacities x LOD / submesh "
              "batches), {:.1f} MB", m_visibleCapacity, m_activeViews, m_visiblePerView, m_visibleCapacity * 4.0 / (1024.0 * 1024.0));
}

void InstanceSystem::prepare(uint32_t slotIndex, FrameData& frame)
{
    if (m_layoutDirty) {
        rebuildLayout();
        m_layoutDirty = false;
    }
    Slot& s = m_slots[slotIndex];
    s.uploads.clear();

    // --- Views: camera, then each directional shadow cascade -------------------------
    auto* views = static_cast<GpuCullView*>(s.views.mapped);
    for (int p = 0; p < 6; ++p) views[0].planes[p] = frame.camera.frustum.planes[p];
    views[0].position = glm::vec4(frame.camera.position, 0.0f);
    uint32_t v = 1;
    for (ShadowView& sv : frame.shadowViews) {
        sv.cullView = kInvalidIndex;
        if (sv.point || v >= kMaxViews) continue; // point-light shadows don't draw instances
        for (int p = 0; p < 6; ++p) views[v].planes[p] = sv.frustum.planes[p];
        // Casters smaller than ~2 texels of this cascade can't cast a visible shadow.
        views[v].position = glm::vec4(frame.camera.position, sv.texelSize * m_renderer->settings().shadowMinCasterTexels);
        sv.cullView = v++;
    }
    m_activeViews = v;
    s.viewCount = v;
    ensureVisibleCapacity(v * m_visiblePerView);
    s.batchCount = static_cast<uint32_t>(m_batches.size());
    s.batchLods.resize(m_batches.size());
    for (size_t b = 0; b < m_batches.size(); ++b) s.batchLods[b] = static_cast<uint8_t>(m_batches[b].lod);
    m_lodScale = 1.0f / std::tan(frame.camera.fovY * 0.5f);

    // --- Groups -----------------------------------------------------------------------
    auto* groups = static_cast<GpuInstanceGroup*>(s.groups.mapped);
    uint32_t batch = 0;
    uint32_t total = 0;
    for (size_t gi = 0; gi < m_groups.size(); ++gi) {
        InstanceGroup* g = m_groups[gi];
        GpuInstanceGroup& gpu = groups[gi];
        gpu = GpuInstanceGroup{};
        gpu.localSphere = g->m_localSphere;
        gpu.firstInstance = g->m_firstInstance;
        gpu.count = g->drawnCount();
        gpu.lodCount = static_cast<uint32_t>(g->m_desc.lods.size());
        const bool screenSize = g->m_desc.lods[0].screenSize > 0.0f;
        gpu.flags = (g->m_desc.castShadows ? 1u : 0u) | (screenSize ? 2u : 0u);
        gpu.cullDistance = g->m_desc.cullDistance;
        gpu.shadowDistance = g->m_desc.shadowDistance;
        for (uint32_t l = 0; l < gpu.lodCount; ++l) {
            // Threshold in the metric the cull shader uses (see instance_cull.comp).
            const InstanceLod& lod = g->m_desc.lods[l];
            gpu.lodDistances[l] = screenSize ? lod.screenSize
                                             : (lod.maxDistance >= FLT_MAX ? 0.0f : 1.0f / std::max(lod.maxDistance, 1e-3f));
            gpu.lodBatchStart[l] = batch;
            const auto n = static_cast<uint32_t>(g->m_desc.lods[l].mesh->submeshes().size());
            gpu.lodBatchCount[l] = n;
            batch += n;
        }
        total += gpu.count;
    }

    // --- Command template: instanceCount = 0, firstInstance = this batch's visible region
    auto* cmds = static_cast<DrawIndexedIndirect*>(s.commands.mapped);
    for (uint32_t view = 0; view < v; ++view) {
        for (uint32_t b = 0; b < m_batches.size(); ++b) {
            const Batch& bt = m_batches[b];
            cmds[view * s.batchCount + b] = {bt.indexCount, 0, bt.firstIndex, 0, view * m_visiblePerView + bt.visibleOffset};
        }
    }

    // --- Instance uploads (only what changed) ---------------------------------------
    VkDeviceSize bytes = 0;
    for (InstanceGroup* g : m_groups) {
        g->m_dirtyEnd = std::min(g->m_dirtyEnd, g->count());
        if (g->m_dirtyBegin < g->m_dirtyEnd) bytes += VkDeviceSize(g->m_dirtyEnd - g->m_dirtyBegin) * sizeof(GpuInstance);
    }
    if (bytes > s.staging.size) {
        Device* d = &m_renderer->device();
        Buffer old = s.staging;
        d->deferDestroy([d, old]() mutable { d->destroyBuffer(old); });
        s.staging = d->createBuffer(std::max(bytes, s.staging.size * 2), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                    MemoryUsage::CpuToGpu, "instance staging");
    }
    VkDeviceSize offset = 0;
    for (InstanceGroup* g : m_groups) {
        if (g->m_dirtyBegin >= g->m_dirtyEnd) {
            g->m_dirtyBegin = g->m_dirtyEnd = 0;
            continue;
        }
        const VkDeviceSize size = VkDeviceSize(g->m_dirtyEnd - g->m_dirtyBegin) * sizeof(GpuInstance);
        std::memcpy(static_cast<uint8_t*>(s.staging.mapped) + offset, &g->m_data[g->m_dirtyBegin], size);
        s.uploads.push_back({offset, VkDeviceSize(g->m_firstInstance + g->m_dirtyBegin) * sizeof(GpuInstance), size});
        offset += size;
        g->m_dirtyBegin = g->m_dirtyEnd = 0;
    }

    m_stats.groups = static_cast<uint32_t>(m_groups.size());
    m_stats.instances = total;
    m_stats.batches = s.batchCount;
}

void InstanceSystem::recordCulling(VkCommandBuffer cmd, uint32_t slotIndex)
{
    Slot& s = m_slots[slotIndex];
    m_drawsThisFrame = 0;
    if (m_batches.empty()) return;

    // Previous frames read these buffers (indirect draws, vertex shaders,
    // stats copy): wait for that before overwriting them.
    vk::memoryBarrier(cmd,
                  VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                  VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    // 1. Upload changed instances.
    if (!s.uploads.empty()) {
        std::vector<VkBufferCopy>& regions = m_copyRegions;
        regions.clear();
        for (const auto& u : s.uploads) regions.push_back({u.src, u.dst, u.size});
        vkCmdCopyBuffer(cmd, s.staging.handle, m_instances.handle, static_cast<uint32_t>(regions.size()), regions.data());
    }
    // 2. Reset the indirect commands (instanceCount = 0).
    const VkBufferCopy reset{0, 0, VkDeviceSize(s.viewCount) * s.batchCount * sizeof(DrawIndexedIndirect)};
    vkCmdCopyBuffer(cmd, s.commands.handle, m_commands.handle, 1, &reset);
    vk::memoryBarrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    // 3. Cull: one dispatch per group, one thread per instance (it tests every view).
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_cullPipeline);
    for (uint32_t gi = 0; gi < m_groups.size(); ++gi) {
        const uint32_t count = m_groups[gi]->drawnCount();
        if (count == 0) continue;
        const RendererSettings& rs = m_renderer->settings();
        CullPush push{gi,          s.batchCount, m_lodScale, rs.lodBias, rs.shadowLodBias,
                      rs.lodCrossfade ? rs.lodFadeBand : 0.0f, s.viewCount, m_hizValid ? 1u : 0u, {}};
        std::copy(m_hiz.begin(), m_hiz.end(), push.hiz);
        vkCmdPushConstants(cmd, m_renderer->pipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (count + 63) / 64, 1, 1);
    }

    // 4. Make the results visible to indirect draws, vertex shaders and the stats copy.
    vk::memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                      VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                  VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                      VK_ACCESS_2_TRANSFER_READ_BIT);
}

void InstanceSystem::draw(RenderContext& ctx, uint32_t cullView, PassKind kind, uint32_t shadowViewIndex)
{
    if (m_batches.empty() || cullView >= m_activeViews) return;
    const uint32_t batchCount = static_cast<uint32_t>(m_batches.size());
    VkPipeline bound = VK_NULL_HANDLE;
    const Mesh* boundMesh = nullptr;
    for (uint32_t b : kind == PassKind::Forward ? m_forwardBatches : m_shadowBatches) {
        const Batch& bt = m_batches[b];
        if (bt.group->drawnCount() == 0) continue;

        VkPipeline p = bt.material->shader().pipeline(kind);
        if (!p) continue; // (still compiling)
        if (p != bound) {
            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
            bound = p;
        }
        if (bt.mesh != boundMesh) {
            bt.mesh->bind(ctx.cmd);
            boundMesh = bt.mesh;
        }
        DrawPush push{};
        push.model = glm::mat4(1.0f);
        push.materialIndex = bt.material->gpuIndex();
        push.viewIndex = shadowViewIndex;
        push.flags = kDrawFlagInstanced;
        vkCmdPushConstants(ctx.cmd, m_renderer->pipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
        vkCmdDrawIndexedIndirect(ctx.cmd, m_commands.handle,
                                 VkDeviceSize(cullView * batchCount + b) * sizeof(DrawIndexedIndirect), 1,
                                 sizeof(DrawIndexedIndirect));
        ++m_drawsThisFrame;
    }
}

void InstanceSystem::recordReadback(VkCommandBuffer cmd, uint32_t slotIndex)
{
    Slot& s = m_slots[slotIndex];
    s.hasReadback = false;
    if (m_batches.empty()) return;
    const VkBufferCopy copy{0, 0, VkDeviceSize(s.viewCount) * s.batchCount * sizeof(DrawIndexedIndirect)};
    vkCmdCopyBuffer(cmd, m_commands.handle, s.readback.handle, 1, &copy);
    s.hasReadback = true;
    m_stats.drawCalls = m_drawsThisFrame;
}

void InstanceSystem::collectStats(uint32_t slotIndex)
{
    Slot& s = m_slots[slotIndex];
    if (!s.hasReadback) return;
    vmaInvalidateAllocation(m_renderer->device().allocator(), s.readback.allocation, 0, VK_WHOLE_SIZE);
    const auto* cmds = static_cast<const DrawIndexedIndirect*>(s.readback.mapped);
    uint32_t camera = 0, shadow = 0;
    uint32_t perLod[4] = {};
    for (uint32_t v = 0; v < s.viewCount; ++v) {
        for (uint32_t b = 0; b < s.batchCount; ++b) {
            const uint32_t n = cmds[v * s.batchCount + b].instanceCount;
            if (v == 0) {
                camera += n;
                if (b < s.batchLods.size()) perLod[s.batchLods[b]] += n;
            } else {
                shadow += n;
            }
        }
    }
    std::copy(perLod, perLod + 4, m_stats.visibleLod);
    m_stats.visibleCamera = camera;
    m_stats.visibleShadow = shadow;
}

} // namespace ve
