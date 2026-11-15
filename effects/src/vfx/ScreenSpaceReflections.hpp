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

#include "vfx/TemporalAccumulator.hpp"

#include <ve/render/PipelineHandle.hpp>
#include <ve/render/RenderGraph.hpp>

namespace vfx {

class ScreenSpaceReflectionsPass : public ve::RenderPass {
public:
    explicit ScreenSpaceReflectionsPass(float resolutionScale = 0.5f);
    ~ScreenSpaceReflectionsPass() override;

    void setup(ve::RenderGraph& graph) override;
    void declare(ve::PassBuilder& builder) override;
    void execute(ve::RenderContext& ctx) override;
    void onResize(ve::RenderGraph&) override { temporal.invalidate(); }
    void onGui() override;

    float maxDistance = 40.0f;  // ray length (m)
    float thickness = 0.6f;     // how far behind a depth sample still counts as a hit (m)
    int steps = 32;
    float maxRoughness = 0.6f;  // rougher surfaces get no SSR
    float intensity = 1.0f;
    float edgeFade = 0.1f;      // fraction of the screen to fade reflections over at the edges
    TemporalAccumulator temporal; // accumulates over frames (removes ray-step noise)

private:
    float m_scale;
    ve::TextureId m_buffer = ve::kNoTexture;
    ve::PipelineHandle m_trace;
    ve::PipelineHandle m_apply;
};

} // namespace vfx
