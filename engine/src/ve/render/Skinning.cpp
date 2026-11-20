#include "ve/render/Skinning.hpp"

#include "ve/core/Log.hpp"
#include "ve/gfx/Device.hpp"
#include "ve/render/Renderer.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace ve {

namespace {
struct SkinWeight { // skin.comp
    glm::uvec4 joints;
    glm::vec4 weights;
};
struct SkinPush {
    uint32_t restBase, outputBase, count, matrixBase;
};
} // namespace

void SkinningSystem::init(Renderer& renderer, uint32_t maxVertices, uint32_t maxJointsPerFrame)
{
    m_renderer = &renderer;
    m_maxVertices = std::max(maxVertices, 1u);
    m_maxJoints = std::max(maxJointsPerFrame, 1u);
    Device& d = renderer.device();
    m_rest = d.createBuffer(VkDeviceSize(m_maxVertices) * sizeof(Vertex), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            MemoryUsage::GpuOnly, "skin rest vertices");
    m_weights = d.createBuffer(VkDeviceSize(m_maxVertices) * sizeof(SkinWeight),
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::GpuOnly, "skin weights");
    m_output = d.createBuffer(VkDeviceSize(m_maxVertices) * sizeof(Vertex), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                              MemoryUsage::GpuOnly, "skinned vertices");
    for (uint32_t i = 0; i < Device::kFramesInFlight; ++i)
        m_matrices.push_back(d.createBuffer(VkDeviceSize(m_maxJoints) * sizeof(glm::mat4), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                            MemoryUsage::CpuToGpu, "skin matrices"));
    m_freeRest = {{0, m_maxVertices}};
    m_freeOutput = {{0, m_maxVertices}};
    m_pipeline = renderer.createComputePipeline("skin.comp");
}

void SkinningSystem::shutdown()
{
    if (!m_renderer) return;
    Device& d = m_renderer->device();
    m_renderer->destroyPipeline(m_pipeline);
    d.destroyBuffer(m_rest);
    d.destroyBuffer(m_weights);
    d.destroyBuffer(m_output);
    for (Buffer& b : m_matrices) d.destroyBuffer(b);
    m_matrices.clear();
    m_renderer = nullptr;
}

bool SkinningSystem::allocate(std::vector<Range>& free, uint32_t count, uint32_t& base)
{
    for (size_t i = 0; i < free.size(); ++i) {
        if (free[i].count < count) continue;
        base = free[i].base;
        free[i].base += count;
        free[i].count -= count;
        if (free[i].count == 0) free.erase(free.begin() + ptrdiff_t(i));
        return true;
    }
    return false;
}

void SkinningSystem::release(std::vector<Range>& free, uint32_t base, uint32_t count)
{
    free.push_back({base, count});
    std::sort(free.begin(), free.end(), [](const Range& a, const Range& b) { return a.base < b.base; });
    for (size_t i = 0; i + 1 < free.size();) { // merge neighbours
        if (free[i].base + free[i].count == free[i + 1].base) {
            free[i].count += free[i + 1].count;
            free.erase(free.begin() + ptrdiff_t(i) + 1);
        } else {
            ++i;
        }
    }
}

SkinningSystem::Instance SkinningSystem::create(const std::shared_ptr<const SkinnedMeshSource>& source)
{
    const SkinnedMeshSource& src = *source;
    const auto count = uint32_t(src.data.vertices.size());
    if (count == 0 || src.joints.size() != count || src.weights.size() != count)
        throw std::runtime_error("Skinned mesh '" + src.name + "': vertices, joints and weights don't match");
    Instance inst;
    inst.count = count;
    inst.source = &src;
    // Rest vertices + weights: once per source.
    auto it = m_sources.find(&src);
    if (it == m_sources.end()) {
        uint32_t base = 0;
        if (!allocate(m_freeRest, count, base))
            throw std::runtime_error("Skinned meshes need more than RendererSettings::maxSkinnedVertices (" + std::to_string(m_maxVertices) + ")");
        std::vector<SkinWeight> w(count);
        for (uint32_t i = 0; i < count; ++i) w[i] = {src.joints[i], src.weights[i]};
        Device& d = m_renderer->device();
        const Device::BufferUpload uploads[] = {
            {&m_rest, src.data.vertices.data(), VkDeviceSize(count) * sizeof(Vertex), VkDeviceSize(base) * sizeof(Vertex)},
            {&m_weights, w.data(), VkDeviceSize(count) * sizeof(SkinWeight), VkDeviceSize(base) * sizeof(SkinWeight)}};
        d.uploadBuffers(uploads);
        it = m_sources.emplace(&src, SourceEntry{base, count, 0}).first;
        m_keepAlive.push_back(source);
    }
    ++it->second.users;
    inst.restBase = it->second.base;
    if (!allocate(m_freeOutput, count, inst.outputBase)) {
        if (--it->second.users == 0) {
            release(m_freeRest, it->second.base, it->second.count);
            m_sources.erase(it);
        }
        throw std::runtime_error("Skinned meshes need more than RendererSettings::maxSkinnedVertices (" + std::to_string(m_maxVertices) + ")");
    }
    // Start from the rest pose (drawn correctly before the first skinning pass).
    m_renderer->device().uploadBuffer(m_output, src.data.vertices.data(), VkDeviceSize(count) * sizeof(Vertex),
                                      VkDeviceSize(inst.outputBase) * sizeof(Vertex));
    inst.mesh = Mesh::createShared(*m_renderer, m_output.handle, VkDeviceSize(inst.outputBase) * sizeof(Vertex), src.data, src.submeshes,
                                   src.name);
    return inst;
}

void SkinningSystem::destroy(Instance& inst)
{
    if (!inst.source || !m_renderer) return;
    // Frames still in flight may skin into / draw from these ranges: only
    // return them to the free lists once the GPU is done (deferred deletion),
    // or a create() could reuse and overwrite them under a running frame.
    // The SkinningSystem outlives the deletion queue (Renderer::shutdown flushes it first).
    const uint32_t outputBase = inst.outputBase, count = inst.count;
    uint32_t restBase = 0, restCount = 0;
    auto it = m_sources.find(inst.source);
    if (it != m_sources.end() && --it->second.users == 0) {
        restBase = it->second.base;
        restCount = it->second.count;
        m_keepAlive.erase(std::remove_if(m_keepAlive.begin(), m_keepAlive.end(), [&](const auto& p) { return p.get() == inst.source; }),
                          m_keepAlive.end());
        m_sources.erase(it); // a new create() of this source uploads a fresh rest range
    }
    m_renderer->device().deferDestroy([this, outputBase, count, restBase, restCount] {
        release(m_freeOutput, outputBase, count);
        if (restCount) release(m_freeRest, restBase, restCount);
    });
    inst = Instance{};
}

void SkinningSystem::beginFrame(uint32_t slot)
{
    m_slot = slot;
    m_jobs.clear();
    m_matrixCount = 0;
}

bool SkinningSystem::add(const Instance& inst, const glm::mat4* joints, uint32_t jointCount)
{
    if (!inst.source || m_matrixCount + jointCount > m_maxJoints) return false;
    std::memcpy(static_cast<glm::mat4*>(m_matrices[m_slot].mapped) + m_matrixCount, joints, jointCount * sizeof(glm::mat4));
    m_jobs.push_back({inst.restBase, inst.outputBase, inst.count, m_matrixCount});
    m_matrixCount += jointCount;
    return true;
}

void SkinningSystem::record(VkCommandBuffer cmd)
{
    if (m_jobs.empty()) return;
    // Last frame's draws read the output; this frame's skinning overwrites it.
    vk::memoryBarrier(cmd, VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    for (const Job& j : m_jobs) {
        const SkinPush push{j.restBase, j.outputBase, j.count, j.matrixBase};
        vkCmdPushConstants(cmd, m_renderer->pipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (j.count + 63) / 64, 1, 1);
    }
    vk::memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                      VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}

} // namespace ve
