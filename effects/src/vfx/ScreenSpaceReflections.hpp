#pragma once

// =============================================================================
// ScreenSpaceReflectionsPass - reflections of what's on screen, for smooth
// surfaces (wet ground, polished stone, metal). Optional; not added by
// addDefaultEffects():
//
//   renderer.graph().addPass<vfx::ScreenSpaceReflectionsPass>();
//
// Traces at reduced resolution against SceneDepth, using SceneNormal (the thin
// G-buffer: world normal + roughness, written by opaque shaders via
// veGBuffer()) and samples the lit SceneColor at the hit. Runs in
// RenderStage::AfterOpaque (after the sky, before transparent geometry and
// fog). Surfaces rougher than maxRoughness are skipped. Reflections of things
// off screen are missing by design (the shader's own sky/ambient specular
// remains).
// =============================================================================

#include "vfx/ScreenSpaceTrace.hpp"

namespace vfx {

/// Screen-space reflections on smooth surfaces (uses the thin G-buffer's roughness).
class ScreenSpaceReflectionsPass : public ScreenSpaceTracePass {
public:
    /// @param resolutionScale trace resolution relative to the screen.
    explicit ScreenSpaceReflectionsPass(float resolutionScale = 0.5f);
    void onGui() override;

    float maxDistance = 40.0f;  ///< ray length (m)
    float thickness = 0.6f;     ///< how far behind a depth sample still counts as a hit (m)
    int steps = 32;  ///< Ray steps per pixel.
    float maxRoughness = 0.6f;  ///< rougher surfaces get no SSR
    float intensity = 1.0f;  ///< Reflection strength.
    float edgeFade = 0.1f;      ///< fraction of the screen to fade reflections over at the edges

protected:
    void pushTrace(ve::RenderContext& ctx, const Inputs& in) override;
    void pushApply(ve::RenderContext& ctx, const Inputs& in, uint32_t result, glm::vec2 size) override;
};

} // namespace vfx
