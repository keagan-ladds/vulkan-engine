#pragma once

// =============================================================================
// PickingPass - "which object is under the mouse?" for the editor.
//
// request(pixel) renders every visible MeshRenderer (batched static ones too,
// from the StaticBatcher) with its draw index as a colour (scissored to that
// one pixel) into an R32F target, copies the pixel
// to a CPU buffer, and a couple of frames later result() returns the owning
// GameObject's id (0 = nothing). Disabled (free) when no pick is pending.
// =============================================================================

#include "ve/gfx/Resources.hpp"
#include "ve/render/RenderGraph.hpp"

#include <optional>
#include <vector>

namespace ve {

class PickingPass : public RenderPass {
public:
    PickingPass();
    ~PickingPass() override;

    void setup(RenderGraph& graph) override;
    void declare(PassBuilder& builder) override;
    void execute(RenderContext& ctx) override;

    void request(glm::ivec2 pixel);
    bool pending() const { return m_state != State::Idle; }
    /// The picked object id (0 = background) once, after a request completed.
    std::optional<uint64_t> result();

private:
    enum class State { Idle, Requested, Rendered };
    State m_state = State::Idle;
    glm::ivec2 m_pixel{0};
    uint64_t m_renderedFrame = 0;
    std::vector<uint64_t> m_owners; ///< draw index - 1 -> object id
    TextureId m_ids = kNoTexture, m_depth = kNoTexture;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    Buffer m_readback;
};

} // namespace ve
