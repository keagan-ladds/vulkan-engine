#pragma once

// =============================================================================
// DepthOfFieldPass - camera focus blur. Optional; not added by
// addDefaultEffects():
//
//   auto& dof = renderer.graph().addPass<vfx::DepthOfFieldPass>();
//   dof.autoFocus = false; dof.focusDistance = 3.0f; // e.g. an aim-down-sights blur
//
// Downsamples colour + linear depth, gathers a golden-angle bokeh at reduced
// resolution, then blends it over the scene by each pixel's circle of
// confusion. Runs at the start of RenderStage::PostProcess (before bloom, on
// HDR colour). Auto focus uses the depth at the screen centre.
// =============================================================================

#include <ve/render/RenderGraph.hpp>

namespace vfx {

class DepthOfFieldPass : public ve::RenderPass {
public:
    explicit DepthOfFieldPass(float resolutionScale = 0.5f);
    ~DepthOfFieldPass() override;

    void setup(ve::RenderGraph& graph) override;
    void declare(ve::PassBuilder& builder) override;
    void execute(ve::RenderContext& ctx) override;
    void onGui() override;

    bool autoFocus = true;
    float focusDistance = 10.0f; // m, used when autoFocus is off
    float focusRange = 1.0f;     // depth range around the focus that stays fully sharp (m)
    float aperture = 2.0f;       // blur strength: CoC = aperture * |1/focus - 1/depth| (1 = full maxBlur)
    float nearBlur = 1.0f;       // near-field blur multiplier (0 = no foreground blur)
    float maxBlur = 12.0f;       // max blur radius (pixels at full resolution)
    int taps = 24;

private:
    float m_scale;
    ve::TextureId m_prep = ve::kNoTexture;   // rgb colour, a linear depth
    ve::TextureId m_buffer = ve::kNoTexture; // blurred colour
    VkPipeline m_prepPipeline = VK_NULL_HANDLE;
    VkPipeline m_gather = VK_NULL_HANDLE;
    VkPipeline m_apply = VK_NULL_HANDLE;
};

} // namespace vfx
