#pragma once

// =============================================================================
// VolumetricFogPass - raymarched exponential height fog with sun shafts
// (sampling the sun's shadow cascades), sky-coloured ambient and glow around
// nearby point lights.
//
// Two steps: raymarch at reduced resolution into "FogBuffer" (rgb = light
// scattered towards the camera, a = transmittance), then upsample and blend
// over SceneColor. Runs at the end of RenderStage::Transparent, so water and
// particles get fogged too. Requires AtmospherePass (uses its SkyViewLUT and
// cloud noise).
// =============================================================================

#include <ve/render/RenderGraph.hpp>

namespace vfx {

class VolumetricFogPass : public ve::RenderPass {
public:
    // resolutionScale: 0.5 = half resolution (4x cheaper), 1.0 = full.
    explicit VolumetricFogPass(float resolutionScale = 0.5f);
    ~VolumetricFogPass() override;

    void setup(ve::RenderGraph& graph) override;
    void prepare(ve::FrameData& frame) override;
    void declare(ve::PassBuilder& builder) override;
    void execute(ve::RenderContext& ctx) override;
    void onGui() override;

    float density = 0.004f;      // extinction per meter at baseHeight
    float heightFalloff = 0.06f; // how quickly fog thins with altitude
    float baseHeight = 0.0f;     // world Y where the fog is densest
    float noise = 0.6f;          // 0 = uniform, 1 = very patchy
    float anisotropy = 0.55f;    // forward scattering: glow towards the sun
    float maxDistance = 900.0f;  // fog ray length for sky pixels
    int steps = 24;
    float ambient = 0.5f;
    float sunShafts = 0.4f;
    glm::vec3 albedo{1.0f, 0.97f, 0.92f};
    float pointLightGlow = 1.0f;
    glm::vec2 windVelocity{1.5f, 0.6f}; // drift of the noise (m/s)

private:
    float m_scale;
    glm::vec2 m_wind{0.0f};
    ve::TextureId m_fog = ve::kNoTexture;
    ve::TextureId m_lut = ve::kNoTexture;
    uint32_t m_noise = 0xFFFFFFFFu;
    VkPipeline m_march = VK_NULL_HANDLE;
    VkPipeline m_apply = VK_NULL_HANDLE;
};

} // namespace vfx
