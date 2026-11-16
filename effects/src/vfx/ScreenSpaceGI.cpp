#include "vfx/ScreenSpaceGI.hpp"

#include <imgui.h>

namespace vfx {

namespace {
struct TracePush {
    glm::vec4 params;
    uint32_t depthTex;
    uint32_t normalTex;
    uint32_t colorTex;
    uint32_t rays;
};
struct ApplyPush {
    glm::vec2 size;
    uint32_t ssgi;
    uint32_t depthTex;
    uint32_t normalTex;
};
} // namespace

ScreenSpaceGIPass::ScreenSpaceGIPass(float resolutionScale)
    // Depth-aware 4x4 upsample; blend: dst = src.rgb + dst * src.a.
    : ScreenSpaceTracePass("Screen-space GI", 0, resolutionScale,
                           {"SSGI", "ssgi.frag", "ssgi_apply.frag", ve::BlendMode::InscatterTransmittance})
{
}

void ScreenSpaceGIPass::pushTrace(ve::RenderContext& ctx, const Inputs& in)
{
    ctx.push(TracePush{{radius, intensity, aoStrength, static_cast<float>(steps)},
                       in.depth,
                       in.normal,
                       in.color,
                       static_cast<uint32_t>(rays)});
}

void ScreenSpaceGIPass::pushApply(ve::RenderContext& ctx, const Inputs& in, uint32_t result, glm::vec2 size)
{
    ctx.push(ApplyPush{size, result, in.depth, in.normal});
}

void ScreenSpaceGIPass::onGui()
{
    ImGui::SliderFloat("Bounce intensity", &intensity, 0.0f, 4.0f);
    ImGui::SliderFloat("AO strength", &aoStrength, 0.0f, 1.0f);
    ImGui::SliderFloat("Radius", &radius, 0.25f, 10.0f);
    ImGui::SliderInt("Rays", &rays, 1, 16);
    ImGui::SliderInt("Steps", &steps, 2, 32);
    ImGui::Checkbox("Temporal accumulation", &temporal.enabled);
    if (temporal.enabled) ImGui::SliderFloat("New frame weight", &temporal.blend, 0.02f, 1.0f);
}

} // namespace vfx
