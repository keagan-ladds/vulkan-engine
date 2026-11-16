#include "vfx/ScreenSpaceReflections.hpp"

#include <imgui.h>

namespace vfx {

namespace {
struct TracePush {
    glm::vec4 params;
    glm::vec4 params2;
    uint32_t depthTex;
    uint32_t normalTex;
    uint32_t colorTex;
};
struct ApplyPush {
    glm::vec2 texel;
    uint32_t ssr;
};
} // namespace

ScreenSpaceReflectionsPass::ScreenSpaceReflectionsPass(float resolutionScale)
    // Premultiplied: rgb = reflection * weight, a = weight.
    : ScreenSpaceTracePass("Screen-space reflections", 10, resolutionScale,
                           {"SSR", "ssr.frag", "ssr_apply.frag", ve::BlendMode::Premultiplied})
{
    temporal.blend = 0.2f; // reflections move with parallax: keep less history than GI
}

void ScreenSpaceReflectionsPass::pushTrace(ve::RenderContext& ctx, const Inputs& in)
{
    ctx.push(TracePush{{maxDistance, thickness, static_cast<float>(steps), maxRoughness},
                       {intensity, edgeFade, 0.0f, 0.0f},
                       in.depth,
                       in.normal,
                       in.color});
}

void ScreenSpaceReflectionsPass::pushApply(ve::RenderContext& ctx, const Inputs&, uint32_t result, glm::vec2 size)
{
    ctx.push(ApplyPush{1.0f / size, result});
}

void ScreenSpaceReflectionsPass::onGui()
{
    ImGui::SliderFloat("Intensity", &intensity, 0.0f, 2.0f);
    ImGui::SliderFloat("Max distance", &maxDistance, 2.0f, 200.0f);
    ImGui::SliderFloat("Thickness", &thickness, 0.05f, 5.0f);
    ImGui::SliderInt("Steps", &steps, 8, 128);
    ImGui::SliderFloat("Max roughness", &maxRoughness, 0.05f, 1.0f);
    ImGui::SliderFloat("Edge fade", &edgeFade, 0.0f, 0.5f);
    ImGui::Checkbox("Temporal accumulation", &temporal.enabled);
    if (temporal.enabled) ImGui::SliderFloat("New frame weight", &temporal.blend, 0.02f, 1.0f);
}

} // namespace vfx
