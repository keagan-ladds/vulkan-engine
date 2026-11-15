#pragma once

// =============================================================================
// ScreenSpaceGIPass - one bounce of diffuse light plus ambient occlusion,
// from what's on screen (sunlit ground brightening the underside of a hut's
// roof, firelight spilling onto nearby walls, contact shadows in crevices).
// Optional; not added by addDefaultEffects():
//
//   renderer.graph().addPass<vfx::ScreenSpaceGIPass>();
//
// Short rays are marched at reduced resolution against SceneDepth/SceneNormal;
// hits gather the lit SceneColor. The result is blurred while upsampling (an
// exact, depth-aware 4x4 average that cancels the Bayer ray rotation) and
// applied as: scene = scene * ao + bounce. Runs in RenderStage::AfterOpaque
// before ScreenSpaceReflectionsPass (so reflections include the bounce).
// The thin G-buffer has no albedo, so the bounce is tinted by the receiving
// pixel's own colour (an approximation).
// =============================================================================

#include "vfx/TemporalAccumulator.hpp"

#include <ve/render/RenderGraph.hpp>

namespace vfx {

class ScreenSpaceGIPass : public ve::RenderPass {
public:
    explicit ScreenSpaceGIPass(float resolutionScale = 0.5f);
    ~ScreenSpaceGIPass() override;

    void setup(ve::RenderGraph& graph) override;
    void declare(ve::PassBuilder& builder) override;
    void execute(ve::RenderContext& ctx) override;
    void onResize(ve::RenderGraph&) override { temporal.invalidate(); }
    void onGui() override;

    float radius = 2.5f;     // ray length (m)
    float intensity = 1.0f;  // bounce light strength
    float aoStrength = 0.7f; // 0 = no occlusion
    int rays = 3;            // per pixel per frame (x 16 rotations in the 4x4 upsample, x ~10 frames accumulated)
    int steps = 8;           // per ray
    TemporalAccumulator temporal; // accumulates over frames (much less noise); temporal.enabled = false to disable

private:
    float m_scale;
    ve::TextureId m_buffer = ve::kNoTexture;
    VkPipeline m_trace = VK_NULL_HANDLE;
    VkPipeline m_apply = VK_NULL_HANDLE;
};

} // namespace vfx
