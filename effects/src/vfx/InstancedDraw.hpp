#pragma once

// =============================================================================
// InstancedDraw - shared plumbing for effect passes that stream per-instance
// data from the CPU every frame (ParticlePass, FirePass):
//
//   ve::Device::kFramesInFlight host-visible vertex buffers, one per frame in
//   flight (the CPU writes this frame's while the GPU may still read the
//   previous one), and a pipeline that reads one per-instance vertex binding,
//   draws into SceneColor and depth-tests (read-only) against the scene.
//
//   vfx::FrameBuffers m_instances;
//   m_instances.create(device, sizeof(Instance) * kMax, "fire instances");
//   VkPipeline p = vfx::buildInstancedPipeline(r, "fire.vert", "fire.frag", binding, attrs, ve::BlendMode::Additive);
//   auto* out = static_cast<Instance*>(m_instances[vfx::FrameBuffers::slot(r)].mapped);
// =============================================================================

#include <ve/gfx/Device.hpp>
#include <ve/gfx/Pipeline.hpp>
#include <ve/render/Renderer.hpp>

#include <array>
#include <span>

namespace vfx {

/// One host-visible buffer per frame in flight (per-frame instance data).
struct FrameBuffers {
    std::array<ve::Buffer, ve::Device::kFramesInFlight> buffers{};  ///< The buffers, by frame slot.

    /// Creates the buffers (`bytes` each).
    void create(ve::Device& device, VkDeviceSize bytes, const char* name)
    {
        for (ve::Buffer& b : buffers)
            b = device.createBuffer(bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, ve::MemoryUsage::CpuToGpu, name);
    }
    /// Deferred: the GPU may still read them.
    void destroy(ve::Device& device)
    {
        ve::Device* d = &device;
        for (ve::Buffer b : buffers) d->deferDestroy([d, b]() mutable { d->destroyBuffer(b); });
        buffers = {};
    }
    /// Index of this frame's buffer.
    static uint32_t slot(const ve::Renderer& r) { return uint32_t(r.frameNumber() % ve::Device::kFramesInFlight); }
    /// The buffer of frame slot `slot`.
    ve::Buffer& operator[](uint32_t slot) { return buffers[slot]; }
};

inline VkPipeline buildInstancedPipeline(ve::Renderer& r, const char* vertex, const char* fragment,
                                         const VkVertexInputBindingDescription& binding,
                                         std::span<const VkVertexInputAttributeDescription> attributes, ve::BlendMode blend)
{
    VkShaderModule vs = r.loadShader(vertex);
    VkShaderModule fs = r.loadShader(fragment);
    VkPipeline p = ve::PipelineBuilder()
                       .shaders(vs, fs)
                       .vertexInput({&binding, 1}, attributes)
                       .colorAttachment(ve::Renderer::kSceneColorFormat, blend)
                       .depthAttachment(ve::Renderer::kDepthFormat)
                       .depthTest(true, false, VK_COMPARE_OP_GREATER_OR_EQUAL) // reversed-Z, no depth writes
                       .layout(r.pipelineLayout())
                       .build(r.device().handle());
    vkDestroyShaderModule(r.device().handle(), vs, nullptr);
    vkDestroyShaderModule(r.device().handle(), fs, nullptr);
    return p;
}

} // namespace vfx
