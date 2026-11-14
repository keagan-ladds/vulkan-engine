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
// hits gather the lit SceneColor. The result is blurred while upsampling and
// applied as: scene = scene * ao + bounce. Runs in RenderStage::AfterOpaque
// before ScreenSpaceReflectionsPass (so reflections include the bounce).
// The thin G-buffer has no albedo, so the bounce is tinted by the receiving
// pixel's own colour (an approximation).
// =============================================================================

#include <ve/render/RenderGraph.hpp>

namespace vfx {

class ScreenSpaceGIPass : public ve::RenderPass {
public:
    explicit ScreenSpaceGIPass(float resolutionScale = 0.5f);
    ~ScreenSpaceGIPass() override;

    void setup(ve::RenderGraph& graph) override;
    void declare(ve::PassBuilder& builder) override;
    void execute(ve::RenderContext& ctx) override;
    void onGui() override;

    float radius = 2.5f;     // ray length (m)
    float intensity = 1.0f;  // bounce light strength
    float aoStrength = 0.7f; // 0 = no occlusion
    int rays = 4;            // per pixel (x 16 rotations through the 4x4 blur)
    int steps = 8;           // per ray

private:
    float m_scale;
    ve::TextureId m_buffer = ve::kNoTexture;
    VkPipeline m_trace = VK_NULL_HANDLE;
    VkPipeline m_apply = VK_NULL_HANDLE;
};

} // namespace vfx
