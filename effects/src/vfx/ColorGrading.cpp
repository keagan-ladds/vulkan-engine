#include "vfx/ColorGrading.hpp"

#include <ve/render/Renderer.hpp>
#include <ve/render/Texture.hpp>
#include <ve/render/passes/BuiltinPasses.hpp>

#include <imgui.h>

#include <cmath>

namespace vfx {

namespace {
struct GradingPush { // color_grading.frag
    glm::vec4 whiteBalance, filter, shadows, highlights, lift, gamma, gain;
    uint32_t sceneColor, tonemapper, lut, lutSize;
};
static_assert(sizeof(GradingPush) <= 128);

// White balance as LMS channel scales (Unity's ColorBalanceToLMSCoeffs).
glm::vec3 whiteBalanceCoeffs(float temperature, float tint)
{
    const float t1 = temperature / 65.0f, t2 = tint / 65.0f;
    const float x = 0.31271f - t1 * (t1 < 0.0f ? 0.1f : 0.05f);
    const float y = 2.87f * x - 3.0f * x * x - 0.27509507f + t2 * 0.05f;
    auto xyToLms = [](float cx, float cy) {
        const float Y = 1.0f, X = Y * cx / cy, Z = Y * (1.0f - cx - cy) / cy;
        return glm::vec3(0.7328f * X + 0.4296f * Y - 0.1624f * Z, -0.7036f * X + 1.6975f * Y + 0.0061f * Z,
                         0.0030f * X + 0.0136f * Y + 0.9834f * Z);
    };
    const glm::vec3 d65(0.949237f, 1.03542f, 1.08728f);
    return d65 / xyToLms(x, y);
}
} // namespace

ColorGradingSettings ColorGradingSettings::sunBleached()
{
    ColorGradingSettings s;
    s.temperature = 14.0f;
    s.tint = 3.0f;
    s.colorFilter = {1.0f, 0.98f, 0.92f};
    s.contrast = 1.32f;
    s.saturation = 1.06f;
    s.vibrance = 0.12f;
    s.tonemapper = Tonemapper::Filmic;
    s.whitePoint = 11.2f;
    s.exposureEV = 0.55f;
    s.shadowsTint = {0.95f, 1.0f, 1.03f};    // olive-teal shade
    s.highlightsTint = {1.07f, 1.0f, 0.86f}; // golden, sun-bleached highlights
    s.splitBalance = 0.0f;
    s.lift = {0.008f, 0.005f, 0.0f};         // a touch of warm haze in the blacks
    s.gamma = {1.0f, 0.98f, 0.94f};
    s.gain = {1.07f, 1.02f, 0.92f};
    return s;
}

ColorGradingPass::ColorGradingPass()
    : FullscreenPass("Color grading", ve::RenderStage::PostProcess, 100,
                     {.fragment = "color_grading.frag", .output = "GradedColor", .inputs = {"SceneColor"}})
{
}

void ColorGradingPass::setup(ve::RenderGraph& g)
{
    if (g.texture("GradedColor") == ve::kNoTexture)
        g.createTexture("GradedColor", {.format = VK_FORMAT_R16G16B16A16_SFLOAT});
    m_output = g.texture("GradedColor");
    FullscreenPass::setup(g);
}

void ColorGradingPass::prepare(ve::FrameData&)
{
    auto* composite = graph().findPass<ve::CompositePass>();
    const float exposure = (composite ? composite->effectiveExposure() : 1.0f) * std::exp2(settings.exposureEV);
    if (composite) composite->useGradedInput(m_output); // composite: vignette + dither only

    const ColorGradingSettings& s = settings;
    GradingPush p{};
    p.whiteBalance = glm::vec4(whiteBalanceCoeffs(s.temperature, s.tint), exposure);
    p.filter = glm::vec4(s.colorFilter, s.contrast);
    p.shadows = glm::vec4(s.shadowsTint, s.saturation);
    p.highlights = glm::vec4(s.highlightsTint, s.splitBalance);
    p.lift = glm::vec4(s.lift, s.vibrance);
    p.gamma = glm::vec4(s.gamma, s.whitePoint);
    p.gain = glm::vec4(s.gain, s.lutContribution);
    p.sceneColor = inputIndex(0);
    p.tonemapper = static_cast<uint32_t>(s.tonemapper);
    p.lut = 0xFFFFFFFFu;
    p.lutSize = 0;
    if (s.lut && s.lut->height() > 1 && s.lut->width() == s.lut->height() * s.lut->height()) {
        p.lut = s.lut->index();
        p.lutSize = s.lut->height();
    }
    setPush(p);
}

void ColorGradingPass::onGui()
{
    if (ImGui::Combo("Preset", &m_preset, "Neutral\0Sun-bleached\0")) {
        auto lut = settings.lut;
        settings = m_preset == 1 ? ColorGradingSettings::sunBleached() : ColorGradingSettings::neutral();
        settings.lut = lut;
    }
    ColorGradingSettings& s = settings;
    int tm = static_cast<int>(s.tonemapper);
    if (ImGui::Combo("Tonemapper", &tm, "ACES (fitted)\0ACES (Hill)\0AgX\0Filmic (Hable)\0Reinhard\0Khronos Neutral\0None\0"))
        s.tonemapper = static_cast<ColorGradingSettings::Tonemapper>(tm);
    if (s.tonemapper == ColorGradingSettings::Tonemapper::Filmic || s.tonemapper == ColorGradingSettings::Tonemapper::Reinhard)
        ImGui::SliderFloat("White point", &s.whitePoint, 1.0f, 20.0f);
    ImGui::SliderFloat("Exposure (EV)", &s.exposureEV, -3.0f, 3.0f);
    ImGui::SliderFloat("Temperature", &s.temperature, -100.0f, 100.0f);
    ImGui::SliderFloat("Tint", &s.tint, -100.0f, 100.0f);
    ImGui::ColorEdit3("Colour filter", &s.colorFilter.x);
    ImGui::SliderFloat("Contrast", &s.contrast, 0.5f, 2.0f);
    ImGui::SliderFloat("Saturation", &s.saturation, 0.0f, 2.0f);
    ImGui::SliderFloat("Vibrance", &s.vibrance, -1.0f, 1.0f);
    ImGui::SeparatorText("Split toning");
    ImGui::ColorEdit3("Shadows", &s.shadowsTint.x);
    ImGui::ColorEdit3("Highlights", &s.highlightsTint.x);
    ImGui::SliderFloat("Balance", &s.splitBalance, -1.0f, 1.0f);
    ImGui::SeparatorText("Lift / gamma / gain");
    ImGui::SliderFloat3("Lift", &s.lift.x, -0.2f, 0.2f);
    ImGui::SliderFloat3("Gamma", &s.gamma.x, 0.5f, 2.0f);
    ImGui::SliderFloat3("Gain", &s.gain.x, 0.5f, 1.5f);
    if (s.lut) ImGui::SliderFloat("LUT contribution", &s.lutContribution, 0.0f, 1.0f);
}

} // namespace vfx
