#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>

namespace ve {

class Renderer;

/// A pipeline from Renderer::createFullscreenPipeline / createComputePipeline.
/// Converts to VkPipeline wherever one is expected; it always yields the
/// current pipeline, so shader hot-reload can swap it underneath. Release it
/// with Renderer::destroyPipeline.
class PipelineHandle {
public:
    PipelineHandle() = default;
    /// An empty handle.
    PipelineHandle(std::nullptr_t) {}
    /// The current pipeline (null if none / destroyed).
    operator VkPipeline() const;
    /// Whether it refers to a pipeline.
    explicit operator bool() const { return m_id != 0; }

private:
    friend class Renderer;
    Renderer* m_renderer = nullptr;
    uint32_t m_id = 0;
};

} // namespace ve
