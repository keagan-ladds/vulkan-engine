#pragma once
// Draws ve::debug lines (ve/debug/Debug.hpp) over the finished image: each
// segment is a screen-space quad (any thickness), depth-tested against
// SceneDepth; the hidden parts are drawn faint.

#include "ve/gfx/Device.hpp"
#include "ve/gfx/Resources.hpp"
#include "ve/render/RenderGraph.hpp"

#include <array>

namespace ve {

class DebugLinePass : public RenderPass {
public:
    DebugLinePass() : RenderPass("Debug lines", RenderStage::Overlay, -50) {}
    ~DebugLinePass() override;
    void setup(RenderGraph& g) override;
    void prepare(FrameData& frame) override;
    void declare(PassBuilder& b) override;
    void execute(RenderContext& ctx) override;
    void onGui() override;

    static constexpr uint32_t kMaxLines = 1u << 17;

private:
    VkPipeline m_visible = VK_NULL_HANDLE; // depth-tested, in front
    VkPipeline m_hidden = VK_NULL_HANDLE;  // depth-tested, behind something: faint
    VkPipeline m_onTop = VK_NULL_HANDLE;   // no depth test
    std::array<Buffer, Device::kFramesInFlight> m_buffers{}; // one per frame in flight
    uint32_t m_slot = 0;
    uint32_t m_tested = 0, m_untested = 0; // this frame's counts (tested first in the buffer)
};

} // namespace ve
