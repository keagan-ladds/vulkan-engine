#pragma once

// =============================================================================
// ColorGradingPass - colour grading and tonemapping (optional; not added by
// addDefaultEffects):
//
//   auto& grade = renderer.graph().addPass<vfx::ColorGradingPass>();
//   grade.settings = vfx::ColorGradingSettings::sunBleached();   // or neutral(), or tweak fields
//   grade.settings.lut = assets.loadTexture("textures/my_grade_lut.png"); // optional; give it a
//   // my_grade_lut.png.texture: {"type": "Data", "mipmaps": false, "compress": false, "wrap": "Clamp"}
//
// Works on HDR SceneColor after bloom/DOF (RenderStage::PostProcess, last):
// exposure (the CompositePass exposure, so day/night adaptation and lightning
// still apply) -> white balance -> colour filter -> contrast -> saturation /
// vibrance -> tonemap (ACES fitted / ACES Hill / AgX / Filmic (Hable) /
// Reinhard / Khronos Neutral / none) -> split toning -> lift / gamma / gain ->
// optional strip LUT. The built-in CompositePass then only adds vignette +
// dither. Disable this pass and the composite's own tonemapping comes back.
//
// LUTs: a "strip" of N slices of N x N (e.g. 1024 x 32), loaded as UNORM (not
// sRGB). Generate a neutral one with tools/generate_assets.py (lut_neutral_32.png),
// grade a screenshot + that strip in any photo editor, apply the same
// adjustments to the strip, and load it.
// =============================================================================

#include <ve/render/passes/FullscreenPass.hpp>

#include <glm/glm.hpp>

#include <memory>

namespace ve {
class Texture;
}

namespace vfx {

struct ColorGradingSettings {
    enum class Tonemapper : int { AcesFitted, AcesHill, AgX, Filmic, Reinhard, KhronosNeutral, None };

    float exposureEV = 0.0f;   // on top of CompositePass::exposure
    float temperature = 0.0f;  // -100 (cool) .. 100 (warm)
    float tint = 0.0f;         // -100 (green) .. 100 (magenta)
    glm::vec3 colorFilter{1.0f};
    float contrast = 1.0f;     // log contrast around mid grey
    float saturation = 1.0f;
    float vibrance = 0.0f;     // -1..1, affects muted colours most
    Tonemapper tonemapper = Tonemapper::AcesFitted;
    float whitePoint = 6.0f;   // Filmic / Reinhard: input level that maps to white
    // Display-space (after tonemapping):
    glm::vec3 shadowsTint{1.0f};
    glm::vec3 highlightsTint{1.0f};
    float splitBalance = 0.0f; // -1..1: where shadows end and highlights begin
    glm::vec3 lift{0.0f};      // raises blacks (per channel)
    glm::vec3 gamma{1.0f};     // mid-tones (>1 brighter)
    glm::vec3 gain{1.0f};      // scales whites
    std::shared_ptr<ve::Texture> lut;
    float lutContribution = 1.0f;

    static ColorGradingSettings neutral() { return {}; }
    // Sun-bleached heat (a starting point for hot, dusty settings): warm white balance, golden highlights,
    // olive/teal shadows with hazy lifted blacks, punchy contrast, slightly
    // tamed saturation and a 2008-era filmic shoulder.
    static ColorGradingSettings sunBleached();
};

class ColorGradingPass : public ve::FullscreenPass {
public:
    ColorGradingPass();
    void setup(ve::RenderGraph& graph) override;
    void prepare(ve::FrameData& frame) override;
    void onGui() override;

    ColorGradingSettings settings = ColorGradingSettings::neutral();

private:
    ve::TextureId m_output = ve::kNoTexture;
    int m_preset = 0;
};

} // namespace vfx
