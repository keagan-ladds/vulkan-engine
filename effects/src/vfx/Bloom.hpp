#pragma once

// =============================================================================
// BloomPass - "physically based" bloom: SceneColor is progressively
// downsampled into a chain of half-size textures (Bloom0..BloomN), then
// upsampled back with a tent filter, accumulating at each level, and finally
// added onto SceneColor. Bright things (sun, fire, emissive) glow softly.
// =============================================================================

#include <ve/render/PipelineHandle.hpp>
#include <ve/render/RenderGraph.hpp>

#include <vector>

namespace vfx {

/// Bloom: bright parts of the HDR image bleed light around them (a mip-chain blur added back).
class BloomPass : public ve::RenderPass {
public:
    /// @param levels blur mip levels (more = wider glow).
    explicit BloomPass(int levels = 6);
    ~BloomPass() override;

    void setup(ve::RenderGraph& graph) override;
    void declare(ve::PassBuilder& builder) override;
    void execute(ve::RenderContext& ctx) override;
    void onGui() override;

    float intensity = 0.06f;  ///< How much bloom is added.
    float threshold = 1.0f; ///< luminance where bloom starts (soft knee below)
    float knee = 0.6f;  ///< Softness of the threshold (0 = hard cut).
    float radius = 1.0f;  ///< Blur radius multiplier.

private:
    int m_levels;
    std::vector<ve::TextureId> m_chain;
    ve::PipelineHandle m_down;
    ve::PipelineHandle m_up;
    ve::PipelineHandle m_composite;
};

} // namespace vfx
