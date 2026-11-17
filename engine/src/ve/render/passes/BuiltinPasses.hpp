#pragma once

#include <cmath>

// =============================================================================
// The engine's built-in render passes (always present, can be disabled):
//
//   ShadowPass       Shadow      directional cascades + point light cubes
//   OpaquePass       Opaque      clears SceneColor/SceneDepth, draws opaque + alpha-test queues
//   TransparentPass  Transparent draws the transparent queue back to front
//   TemporalAAPass   PostProcess temporal anti-aliasing (RendererSettings::temporalAA), before other post effects
//   CompositePass    Composite   exposure + tonemapping + simple grading -> Backbuffer
//
// Effects (sky, fog, bloom, particles) live in the optional ve_effects module
// and slot in between using RenderStage.
// =============================================================================

#include "ve/render/passes/FullscreenPass.hpp"

namespace ve {

// Uploads changed instances and runs GPU culling for every InstanceGroup
// (camera + directional shadow cascades) before anything is drawn.
class InstanceCullPass : public RenderPass {
public:
    InstanceCullPass() : RenderPass("Instance culling", RenderStage::Shadow, -100) {}
    void declare(PassBuilder&) override {}
    void execute(RenderContext& ctx) override;
    void onGui() override;
};

class ShadowPass : public RenderPass {
public:
    ShadowPass() : RenderPass("Shadows", RenderStage::Shadow) {}
    void declare(PassBuilder& builder) override;
    void execute(RenderContext& ctx) override;
    void onGui() override;

private:
    std::vector<uint32_t> m_scratch;
    std::vector<uint64_t> m_pointFaceHash; // per point-shadow layer: inputs of its last render (0 = none)
    uint32_t m_pointFacesRendered = 0, m_pointFacesCached = 0;
};

class OpaquePass : public RenderPass {
public:
    OpaquePass() : RenderPass("Opaque", RenderStage::Opaque) {}
    void declare(PassBuilder& builder) override;
    void execute(RenderContext& ctx) override;
    glm::vec4 clearColor{0.0f, 0.0f, 0.0f, 1.0f};
};

class TransparentPass : public RenderPass {
public:
    TransparentPass() : RenderPass("Transparent", RenderStage::Transparent) {}
    void declare(PassBuilder& builder) override;
    void execute(RenderContext& ctx) override;
};

// Temporal anti-aliasing: blends the (jittered) frame into a history
// reprojected with the camera motion (from depth), clipped to the current
// 3x3 neighbourhood so moving objects don't leave trails. Only runs while
// RendererSettings::temporalAA is on (which also jitters the camera).
class TemporalAAPass : public RenderPass {
public:
    TemporalAAPass() : RenderPass("Temporal AA", RenderStage::PostProcess, -100) {}
    ~TemporalAAPass() override;
    void setup(RenderGraph& graph) override;
    void declare(PassBuilder& builder) override;
    void execute(RenderContext& ctx) override;
    void onGui() override;

    float feedback = 0.9f; // history weight (higher = smoother, more ghosting)

private:
    TextureId m_history[2] = {kNoTexture, kNoTexture};
    PipelineHandle m_pipeline;
    uint32_t m_frame = 0;
    VkExtent3D m_extent{};
    uint64_t m_lastFrame = 0; // frame number of the last resolve (history is stale after a gap)
    bool m_active = false;
};

// HDR -> display. Settings are public so games can drive them (e.g. dim at night).
class CompositePass : public FullscreenPass {
public:
    CompositePass();
    void prepare(FrameData& frame) override;
    void declare(PassBuilder& builder) override;
    void execute(RenderContext& ctx) override;
    void onGui() override;

    // A grading pass (e.g. vfx::ColorGradingPass) that already applied exposure,
    // grading and tonemapping calls this from its prepare() every frame; the
    // composite then only adds vignette + dither. Cleared after each frame, so
    // disabling the grading pass falls back to the built-in tonemapping.
    void useGradedInput(TextureId texture) { m_graded = texture; }
    float effectiveExposure() const { return exposure * exposureMultiplier * std::exp2(exposureCompensation); }

    float exposure = 1.0f;
    float exposureMultiplier = 1.0f; // transient boosts (lightning flashes); multiplies exposure
    float exposureCompensation = 0.0f; // EV stops on top of everything (photo mode, brightness setting)
    float contrast = 1.05f;
    float saturation = 1.1f;
    float vignette = 0.25f;
    glm::vec3 tint{1.0f}; // colour balance multiplier
    int tonemapper = 0;   // 0 = ACES (fitted), 1 = Reinhard, 2 = none

private:
    TextureId m_graded = kNoTexture;
};

} // namespace ve
