#pragma once

// =============================================================================
// The engine's built-in render passes (always present, can be disabled):
//
//   ShadowPass       Shadow      directional cascades + point light cubes
//   OpaquePass       Opaque      clears SceneColor/SceneDepth, draws opaque + alpha-test queues
//   TransparentPass  Transparent draws the transparent queue back to front
//   CompositePass    Composite   exposure + tonemapping + simple grading -> Backbuffer
//
// Effects (sky, fog, bloom, particles) live in the optional ve_effects module
// and slot in between using RenderStage.
// =============================================================================

#include "ve/render/passes/FullscreenPass.hpp"

namespace ve {

class ShadowPass : public RenderPass {
public:
    ShadowPass() : RenderPass("Shadows", RenderStage::Shadow) {}
    void declare(PassBuilder& builder) override;
    void execute(RenderContext& ctx) override;
    void onGui() override;

private:
    std::vector<uint32_t> m_scratch;
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

// HDR -> display. Settings are public so games can drive them (e.g. dim at night).
class CompositePass : public FullscreenPass {
public:
    CompositePass();
    void prepare(FrameData& frame) override;
    void onGui() override;

    float exposure = 1.0f;
    float contrast = 1.05f;
    float saturation = 1.1f;
    float vignette = 0.25f;
    glm::vec3 tint{1.0f}; // colour balance multiplier
    int tonemapper = 0;   // 0 = ACES (fitted), 1 = Reinhard, 2 = none
};

} // namespace ve
