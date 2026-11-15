#include "vfx/Atmosphere.hpp"

#include "vfx/Noise.hpp"
#include "vfx/Vfx.hpp"

#include <ve/core/Log.hpp>
#include <ve/render/Renderer.hpp>
#include <ve/render/Texture.hpp>
#include <ve/scene/Components.hpp>

#include <imgui.h>

#include <algorithm>

#include <chrono>
#include <cmath>

namespace vfx {

namespace {

// ---- CPU mirror of the atmosphere model in vfx.glsl ---------------------------
constexpr float kEarth = 6360e3f;
constexpr float kAtmos = 6460e3f;
const glm::vec3 kRayleigh(5.802e-6f, 13.558e-6f, 33.1e-6f);
constexpr float kRayleighH = 8000.0f;
constexpr float kMieScatter = 3.996e-6f;
constexpr float kMieExt = 4.440e-6f;
constexpr float kMieH = 1200.0f;
const glm::vec3 kOzone(0.650e-6f, 1.881e-6f, 0.085e-6f);
constexpr float kPi = 3.14159265f;

glm::vec2 raySphere(const glm::vec3& ro, const glm::vec3& rd, float r)
{
    const float b = glm::dot(ro, rd);
    const float c = glm::dot(ro, ro) - r * r;
    float d = b * b - c;
    if (d < 0.0f) return {1e9f, -1e9f};
    d = std::sqrt(d);
    return {-b - d, -b + d};
}

glm::vec3 extinction(float h, float mieScale)
{
    const float ozone = std::max(0.0f, 1.0f - std::abs(h - 25000.0f) / 15000.0f);
    return kRayleigh * std::exp(-h / kRayleighH) + glm::vec3(kMieExt * std::exp(-h / kMieH) * mieScale) + kOzone * ozone;
}

glm::vec3 transmittanceFrom(const glm::vec3& p, const glm::vec3& dir, float haze, int steps)
{
    const glm::vec2 g = raySphere(p, dir, kEarth);
    if (g.x > 0.0f && g.x <= g.y) return glm::vec3(0.0f);
    const float len = raySphere(p, dir, kAtmos).y;
    const float ds = len / static_cast<float>(steps);
    glm::vec3 od(0.0f);
    for (int i = 0; i < steps; ++i) {
        const glm::vec3 q = p + dir * ((static_cast<float>(i) + 0.5f) * ds);
        od += extinction(glm::length(q) - kEarth, haze) * ds;
    }
    return glm::exp(-od);
}

// Push-constant blocks; must match the shaders byte for byte.
struct LutPush {
    glm::vec4 sunDir;
    glm::vec4 params;
    glm::vec4 ground;
};
struct CloudPush {
    glm::vec4 layer;
    glm::vec4 shape;
    glm::vec4 sunColor;
    glm::vec4 ambient;
    glm::vec2 wind;
    uint32_t noiseTex;
    uint32_t depthTex;
    uint32_t skyLut;
};
struct SkyPush {
    glm::vec4 sunDisk;
    glm::vec4 moonDir;
    glm::vec4 night;
    uint32_t skyLut;
    uint32_t clouds;
};

} // namespace

glm::vec3 AtmospherePass::sunTransmittance(const glm::vec3& towardsSun, float haze)
{
    const glm::vec3 p(0.0f, kEarth + 2.0f, 0.0f);
    glm::vec3 dir = glm::normalize(towardsSun);
    // A tiny lift keeps the sun from popping to black exactly at the horizon.
    dir.y = std::max(dir.y, -0.02f) + 0.012f;
    return transmittanceFrom(p, glm::normalize(dir), haze, 24);
}

glm::vec3 AtmospherePass::skyRadiance(const glm::vec3& dir, const glm::vec3& towardsSun, float haze, float mieG,
                                      float multiScatter)
{
    const glm::vec3 ro(0.0f, kEarth + 2.0f, 0.0f);
    const glm::vec2 atmo = raySphere(ro, dir, kAtmos);
    const glm::vec2 ground = raySphere(ro, dir, kEarth);
    float tMax = atmo.y;
    if (ground.x > 0.0f && ground.x <= ground.y) tMax = ground.x;
    const float c = glm::dot(dir, towardsSun);
    const float phaseR = 3.0f / (16.0f * kPi) * (1.0f + c * c);
    const float g2 = mieG * mieG;
    const float phaseM = 3.0f / (8.0f * kPi) * (1.0f - g2) / (2.0f + g2) * (1.0f + c * c) /
                         std::pow(std::max(1.0f + g2 - 2.0f * mieG * c, 1e-4f), 1.5f);
    const int N = 12;
    glm::vec3 radiance(0.0f), throughput(1.0f);
    float prevT = 0.0f;
    for (int i = 0; i < N; ++i) {
        const float t = tMax * std::pow((static_cast<float>(i) + 0.5f) / N, 2.0f);
        const float ds = t - prevT;
        prevT = t;
        const glm::vec3 p = ro + dir * t;
        const float h = glm::length(p) - kEarth;
        const glm::vec3 ext = extinction(h, haze);
        const glm::vec3 sR = kRayleigh * std::exp(-h / kRayleighH);
        const float sM = kMieScatter * std::exp(-h / kMieH) * haze;
        const glm::vec3 sunT = transmittanceFrom(p, towardsSun, haze, 4);
        const float sunVis = (sunT.r + sunT.g + sunT.b) / 3.0f;
        const glm::vec3 in = sunT * (sR * phaseR + glm::vec3(sM * phaseM)) + (sR + glm::vec3(sM)) * multiScatter * 0.08f * sunVis;
        const glm::vec3 stepT = glm::exp(-ext * ds);
        radiance += throughput * in * (glm::vec3(1.0f) - stepT) / glm::max(ext, glm::vec3(1e-9f));
        throughput *= stepT;
    }
    return radiance;
}

AtmospherePass::AtmospherePass() : RenderPass("Atmosphere", ve::RenderStage::Sky) {}

uint32_t AtmospherePass::noiseTextureIndex() const
{
    return m_noise ? m_noise->index() : ve::kInvalidIndex;
}

AtmospherePass::~AtmospherePass()
{
    renderer().destroyPipeline(m_lutPipeline);
    renderer().destroyPipeline(m_cloudPipeline);
    renderer().destroyPipeline(m_skyPipeline);
}

void AtmospherePass::setup(ve::RenderGraph& g)
{
    ve::Renderer& r = renderer();
    m_lut = g.createTexture("SkyViewLUT", {.format = VK_FORMAT_R16G16B16A16_SFLOAT, .width = 192, .height = 108});
    m_clouds = g.createTexture("CloudBuffer", {.format = VK_FORMAT_R16G16B16A16_SFLOAT, .scale = 0.5f});

    m_lutPipeline = r.createFullscreenPipeline({.fragment = "sky_lut.frag", .colorFormats = {VK_FORMAT_R16G16B16A16_SFLOAT}});
    m_cloudPipeline = r.createFullscreenPipeline({.fragment = "clouds.frag", .colorFormats = {VK_FORMAT_R16G16B16A16_SFLOAT}});
    m_skyPipeline = r.createFullscreenPipeline({.fragment = "sky.frag",
                                                .colorFormats = {ve::Renderer::kSceneColorFormat},
                                                .depthFormat = ve::Renderer::kDepthFormat,
                                                .depthCompare = VK_COMPARE_OP_GREATER_OR_EQUAL});

    const auto t0 = std::chrono::steady_clock::now();
    const uint32_t n = 64;
    const auto noise = generateCloudNoise(n);
    m_noise = ve::Texture::create3D(r, n, n, n, VK_FORMAT_R8G8B8A8_UNORM, noise.data(), noise.size(),
                                    ve::SamplerType::LinearRepeat, "cloud noise");
    ve::log::info("Atmosphere: cloud noise generated in {:.0f} ms",
                  std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}

void AtmospherePass::prepare(ve::FrameData& frame)
{
    ve::Renderer& r = renderer();

    // Which way is the sun?
    if (sun) {
        m_sunDir = -glm::normalize(sun->transform().forward());
    } else if (frame.globals.lightCounts.x > 0) {
        m_sunDir = -glm::vec3(frame.lights->directional[0].direction);
    }
    m_cameraHeight = std::max(1.0f, frame.camera.position.y + 2.0f);
    m_windOffset += windVelocity * frame.deltaTime;
    m_windOffset = glm::mod(m_windOffset, glm::vec2(baseScale * 40.0f));

    const glm::vec3 T = sunTransmittance(m_sunDir, haze);
    // Overcast skies block most direct sun and dim/grey the sky.
    m_sunRadiance = T * sunIlluminance * (1.0f - 0.85f * overcast);
    const float night = glm::smoothstep(0.05f, -0.15f, m_sunDir.y);

    // Sky ambient: average radiance of a few sky directions.
    glm::vec3 sky = skyRadiance({0, 1, 0}, m_sunDir, haze, mieAnisotropy, multipleScattering) * 0.4f;
    for (int i = 0; i < 4; ++i) {
        const float a = static_cast<float>(i) * kPi * 0.5f;
        sky += skyRadiance(glm::normalize(glm::vec3(std::cos(a), 0.35f, std::sin(a))), m_sunDir, haze, mieAnisotropy,
                           multipleScattering) * 0.15f;
    }
    m_ambientSky = sky * sunIlluminance * (1.0f - 0.45f * overcast);
    const float grey = (m_ambientSky.r + m_ambientSky.g + m_ambientSky.b) / 3.0f;
    m_ambientSky = glm::mix(m_ambientSky, glm::vec3(grey), overcast * 0.6f);

    r.setGlobalParam(SlotSun, glm::vec4(m_sunDir, sunIlluminance));
    r.setGlobalParam(SlotSunColor, glm::vec4(m_sunRadiance, 0.0f));
    frame.globals.globalParams[SlotSun] = glm::vec4(m_sunDir, sunIlluminance);
    frame.globals.globalParams[SlotSunColor] = glm::vec4(m_sunRadiance, 0.0f);
    glm::vec4 weather = frame.globals.globalParams[SlotWeather];
    weather.z = coverage;
    frame.globals.globalParams[SlotWeather] = weather;

    if (driveAmbient) {
        ve::Environment& env = r.environment();
        const glm::vec3 ambSky = m_ambientSky * ambientScale + nightAmbient * night;
        // Ground bounce = sunlit ground + skylit ground. The sunlit part is
        // reported separately so shaders can remove it where the sun is hidden.
        const glm::vec3 groundSun = groundAlbedo * m_sunRadiance * std::max(m_sunDir.y, 0.0f) * ambientScale;
        const glm::vec3 ambGround = groundSun + groundAlbedo * m_ambientSky * 0.5f * ambientScale + nightAmbient * 0.4f * night;
        auto lum = [](const glm::vec3& c) { return c.r * 0.2126f + c.g * 0.7152f + c.b * 0.0722f; };
        env.ambientSky = ambSky;
        env.ambientGround = ambGround;
        env.groundSunFraction = std::clamp(lum(groundSun) / std::max(lum(ambGround), 1e-6f), 0.0f, 1.0f);
        frame.globals.ambientSky = glm::vec4(ambSky * env.ambientIntensity, 0.0f);
        frame.globals.ambientGround = glm::vec4(ambGround * env.ambientIntensity, env.groundSunFraction);
    }
}

void AtmospherePass::declare(ve::PassBuilder& b)
{
    ve::Renderer& r = renderer();
    b.use(m_lut, ve::Access::ColorAttachment);
    b.use(m_clouds, ve::Access::ColorAttachment);
    b.use(r.sceneDepth(), ve::Access::Sampled);
    b.use(r.sceneDepth(), ve::Access::DepthRead);
    b.use(r.sceneColor(), ve::Access::ColorAttachment);
}

void AtmospherePass::execute(ve::RenderContext& ctx)
{
    ve::Renderer& r = renderer();
    ve::RenderGraph& g = graph();
    const uint32_t lutIndex = g.get(m_lut).sampledIndex;
    const float night = glm::smoothstep(0.05f, -0.15f, m_sunDir.y);

    // 1. Sky-view LUT.
    ctx.beginRendering({{m_lut}});
    ctx.bindPipeline(m_lutPipeline);
    ctx.push(LutPush{glm::vec4(m_sunDir, sunIlluminance * (1.0f - 0.5f * overcast)),
                     glm::vec4(m_cameraHeight, haze, mieAnisotropy, multipleScattering), glm::vec4(groundAlbedo, overcast)});
    ctx.drawFullscreen();
    ctx.endRendering();
    g.transition(ctx.cmd, m_lut, ve::Access::Sampled);

    // 2. Clouds at reduced resolution.
    ctx.beginRendering({{m_clouds, true, glm::vec4(0, 0, 0, 1)}});
    if (clouds) {
        glm::vec3 cloudLight = m_sunRadiance;
        if (moon && moon->isActive()) {
            cloudLight += moon->color * moon->intensity * 0.6f * std::max(0.0f, -moon->transform().forward().y);
        }
        CloudPush p{};
        p.layer = {cloudBottom, cloudTop, coverage, cloudDensity};
        p.shape = {baseScale, detailScale, detailStrength, static_cast<float>(cloudSteps)};
        p.sunColor = glm::vec4(cloudLight, static_cast<float>(cloudLightSteps));
        p.ambient = glm::vec4(m_ambientSky + nightAmbient * night * 2.0f, cloudForwardScattering);
        p.wind = m_windOffset;
        p.noiseTex = m_noise->index();
        p.depthTex = g.get(r.sceneDepth()).sampledIndex;
        p.skyLut = lutIndex;
        ctx.bindPipeline(m_cloudPipeline);
        ctx.push(p);
        ctx.drawFullscreen();
    }
    ctx.endRendering();
    g.transition(ctx.cmd, m_clouds, ve::Access::Sampled);

    // 3. Sky into SceneColor (only where depth == far plane).
    ve::DepthTarget depth;
    depth.texture = r.sceneDepth();
    depth.readOnly = true;
    ctx.beginRendering({{r.sceneColor()}}, depth);
    SkyPush p{};
    const float radius = glm::radians(0.27f * sunDiskSize);
    p.sunDisk = glm::vec4(m_sunRadiance * sunDiskBrightness, std::cos(radius));
    if (moon && moon->isActive()) {
        const glm::vec3 md = -glm::normalize(moon->transform().forward());
        p.moonDir = glm::vec4(md, md.y > -0.05f ? 3.0f * night : 0.0f);
    }
    p.night = glm::vec4(starBrightness * night, night, 0, 0);
    p.skyLut = lutIndex;
    p.clouds = g.get(m_clouds).sampledIndex;
    ctx.bindPipeline(m_skyPipeline);
    ctx.push(p);
    ctx.drawFullscreen();
    ctx.endRendering();
}

void AtmospherePass::onGui()
{
    ImGui::SeparatorText("Sky");
    ImGui::SliderFloat("Illuminance", &sunIlluminance, 0.1f, 40.0f);
    ImGui::SliderFloat("Haze", &haze, 0.0f, 10.0f);
    ImGui::SliderFloat("Mie anisotropy", &mieAnisotropy, 0.0f, 0.95f);
    ImGui::SliderFloat("Multiple scattering", &multipleScattering, 0.0f, 4.0f);
    ImGui::SliderFloat("Sun disc size", &sunDiskSize, 0.5f, 6.0f);
    ImGui::SliderFloat("Stars", &starBrightness, 0.0f, 4.0f);
    ImGui::SliderFloat("Overcast", &overcast, 0.0f, 1.0f);
    ImGui::Checkbox("Drive ambient", &driveAmbient);
    ImGui::SliderFloat("Ambient scale", &ambientScale, 0.0f, 3.0f);
    ImGui::SeparatorText("Clouds");
    ImGui::Checkbox("Enabled", &clouds);
    ImGui::SliderFloat("Coverage", &coverage, 0.0f, 1.0f);
    ImGui::SliderFloat("Density", &cloudDensity, 0.0f, 3.0f);
    ImGui::SliderFloat("Bottom", &cloudBottom, 200.0f, 5000.0f);
    ImGui::SliderFloat("Top", &cloudTop, 500.0f, 8000.0f);
    ImGui::SliderFloat("Size", &baseScale, 1000.0f, 30000.0f);
    ImGui::SliderFloat("Detail strength", &detailStrength, 0.0f, 1.0f);
    ImGui::SliderFloat2("Wind", &windVelocity.x, -50.0f, 50.0f);
    ImGui::SliderInt("Steps", &cloudSteps, 8, 128);
    ImGui::SliderInt("Light steps", &cloudLightSteps, 1, 12);
}

} // namespace vfx
