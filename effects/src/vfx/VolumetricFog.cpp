#include "vfx/VolumetricFog.hpp"

#include "vfx/Atmosphere.hpp"
#include "vfx/Vfx.hpp"

#include <ve/render/Renderer.hpp>

#include <imgui.h>

#include <cstring>

namespace vfx {

namespace {
struct FogPush {
    glm::vec4 density;
    glm::vec4 scatter;
    glm::vec4 sunColor;
    glm::vec4 tint;
    glm::vec2 wind;
    uint32_t depthTex;
    uint32_t skyLut;
    uint32_t noiseTex;
};
} // namespace

VolumetricFogPass::VolumetricFogPass(float resolutionScale)
    : RenderPass("Volumetric fog", ve::RenderStage::Transparent, 50), m_scale(resolutionScale)
{
}

VolumetricFogPass::~VolumetricFogPass()
{
    renderer().destroyPipeline(m_march);
    renderer().destroyPipeline(m_apply);
}

void VolumetricFogPass::setup(ve::RenderGraph& g)
{
    m_lut = g.texture("SkyViewLUT");
    if (m_lut == ve::kNoTexture) throw std::runtime_error("VolumetricFogPass requires AtmospherePass (SkyViewLUT)");
    if (auto* atmo = g.findPass<AtmospherePass>()) m_noise = atmo->noiseTextureIndex();
    m_fog = g.createTexture("FogBuffer", {.format = VK_FORMAT_R16G16B16A16_SFLOAT, .scale = m_scale});

    ve::Renderer& r = renderer();
    m_march = r.createFullscreenPipeline({.fragment = "fog.frag", .colorFormats = {VK_FORMAT_R16G16B16A16_SFLOAT}});
    m_apply = r.createFullscreenPipeline({.fragment = "fog_apply.frag",
                                          .colorFormats = {ve::Renderer::kSceneColorFormat},
                                          .blend = ve::BlendMode::InscatterTransmittance});
}

void VolumetricFogPass::prepare(ve::FrameData& frame)
{
    m_wind += windVelocity * frame.deltaTime;
    // The weather's fog multiplier lives in globalParams[SlotWeather].w;
    // default it to 1 if nobody set it.
    glm::vec4& weather = frame.globals.globalParams[SlotWeather];
    if (weather.w <= 0.0f) weather.w = 1.0f;
}

void VolumetricFogPass::declare(ve::PassBuilder& b)
{
    ve::Renderer& r = renderer();
    b.use(r.sceneDepth(), ve::Access::Sampled);
    b.use(m_lut, ve::Access::Sampled);
    b.use(r.dirShadowMap(), ve::Access::Sampled);
    b.use(m_fog, ve::Access::ColorAttachment);
    b.use(r.sceneColor(), ve::Access::ColorAttachment);
}

void VolumetricFogPass::execute(ve::RenderContext& ctx)
{
    ve::Renderer& r = renderer();
    ve::RenderGraph& g = graph();
    const ve::FrameData& frame = *ctx.frame;

    FogPush p{};
    p.density = {density, heightFalloff, baseHeight, noise};
    p.scatter = {anisotropy, maxDistance, static_cast<float>(steps), ambient};
    p.sunColor = glm::vec4(glm::vec3(frame.globals.globalParams[SlotSunColor]), sunShafts);
    p.tint = glm::vec4(albedo, pointLightGlow);
    p.wind = m_wind;
    p.depthTex = g.get(r.sceneDepth()).sampledIndex;
    p.skyLut = g.get(m_lut).sampledIndex;
    p.noiseTex = m_noise;

    // 1. Raymarch at reduced resolution.
    ctx.beginRendering({{m_fog}});
    ctx.bindPipeline(m_march);
    ctx.push(p);
    ctx.drawFullscreen();
    ctx.endRendering();
    g.transition(ctx.cmd, m_fog, ve::Access::Sampled);

    // 2. Upsample + blend over the scene.
    ctx.beginRendering({{r.sceneColor()}});
    ctx.bindPipeline(m_apply);
    ctx.push(g.get(m_fog).sampledIndex);
    ctx.drawFullscreen();
    ctx.endRendering();
}

void VolumetricFogPass::onGui()
{
    ImGui::SliderFloat("Density", &density, 0.0f, 0.1f, "%.4f", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Height falloff", &heightFalloff, 0.001f, 0.5f, "%.3f", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Base height", &baseHeight, -50.0f, 100.0f);
    ImGui::SliderFloat("Noise", &noise, 0.0f, 1.0f);
    ImGui::SliderFloat("Anisotropy", &anisotropy, 0.0f, 0.95f);
    ImGui::SliderFloat("Sun shafts", &sunShafts, 0.0f, 4.0f);
    ImGui::SliderFloat("Ambient", &ambient, 0.0f, 3.0f);
    ImGui::SliderFloat("Point light glow", &pointLightGlow, 0.0f, 5.0f);
    ImGui::SliderFloat("Max distance", &maxDistance, 50.0f, 3000.0f);
    ImGui::SliderInt("Steps", &steps, 4, 96);
    ImGui::ColorEdit3("Albedo", &albedo.x);
}

} // namespace vfx
