#pragma once

// =============================================================================
// BloomPass - "physically based" bloom: SceneColor is progressively
// downsampled into a chain of half-size textures (Bloom0..BloomN), then
// upsampled back with a tent filter, accumulating at each level, and finally
// added onto SceneColor. Bright things (sun, fire, emissive) glow softly.
// =============================================================================

#include <ve/render/RenderGraph.hpp>

#include <vector>

namespace vfx {

class BloomPass : public ve::RenderPass {
public:
    explicit BloomPass(int levels = 6);
    ~BloomPass() override;

    void setup(ve::RenderGraph& graph) override;
    void declare(ve::PassBuilder& builder) override;
    void execute(ve::RenderContext& ctx) override;
    void onGui() override;

    float intensity = 0.06f;
    float threshold = 1.0f; // luminance where bloom starts (soft knee below)
    float knee = 0.6f;
    float radius = 1.0f;

private:
    int m_levels;
    std::vector<ve::TextureId> m_chain;
    VkPipeline m_down = VK_NULL_HANDLE;
    VkPipeline m_up = VK_NULL_HANDLE;
    VkPipeline m_composite = VK_NULL_HANDLE;
};

} // namespace vfx
