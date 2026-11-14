#pragma once

// =============================================================================
// AtmospherePass - physically based sky, sun/moon discs, stars and
// volumetric clouds. Runs in RenderStage::Sky after opaque geometry and only
// touches pixels where nothing was drawn.
//
// Each frame it:
//   1. renders a small sky-view LUT ("SkyViewLUT", usable by other passes),
//   2. raymarches clouds at reduced resolution ("CloudBuffer"),
//   3. composites sky + sun + stars + clouds into SceneColor.
// In prepare() it also publishes the sun direction/colour as global shader
// params and (optionally) sets the renderer's ambient light from the sky.
//
// The sun's *light* colour isn't changed automatically; use
// AtmospherePass::sunTransmittance() to colour your DirectionalLight (see
// the sandbox DayNightCycle).
// =============================================================================

#include <ve/render/RenderGraph.hpp>

#include <glm/glm.hpp>

#include <memory>

namespace ve {
class DirectionalLight;
class Texture;
} // namespace ve

namespace vfx {

class AtmospherePass : public ve::RenderPass {
public:
    AtmospherePass();
    ~AtmospherePass() override;

    void setup(ve::RenderGraph& graph) override;
    void prepare(ve::FrameData& frame) override;
    void declare(ve::PassBuilder& builder) override;
    void execute(ve::RenderContext& ctx) override;
    void onGui() override;

    // --- Sources ---------------------------------------------------------------
    ve::DirectionalLight* sun = nullptr;  // if null, the brightest directional light
    ve::DirectionalLight* moon = nullptr; // optional, draws a moon disc

    // --- Sky -------------------------------------------------------------------
    float sunIlluminance = 9.0f;  // overall sky brightness
    float haze = 1.5f;            // aerosol (Mie) amount: >1 = dusty savannah air
    float mieAnisotropy = 0.8f;   // size of the glow around the sun
    float multipleScattering = 1.0f;
    float sunDiskSize = 1.0f;     // multiplier on the real 0.27 degree radius
    float sunDiskBrightness = 40.0f;
    float starBrightness = 1.0f;
    float overcast = 0.0f;        // 0..1: blocks direct sun, greys the sky (set by Weather)
    glm::vec3 groundAlbedo{0.3f, 0.25f, 0.18f};

    // --- Ambient ---------------------------------------------------------------
    bool driveAmbient = true;     // set renderer ambient from the sky each frame
    float ambientScale = 1.0f;
    glm::vec3 nightAmbient{0.010f, 0.014f, 0.028f};

    // --- Clouds ----------------------------------------------------------------
    bool clouds = true;
    float cloudBottom = 1400.0f;  // meters
    float cloudTop = 3200.0f;
    float coverage = 0.45f;       // 0 = clear, 1 = overcast
    float cloudDensity = 1.0f;
    float baseScale = 9000.0f;    // meters per noise tile (cloud size)
    float detailScale = 1700.0f;
    float detailStrength = 0.35f;
    float cloudForwardScattering = 0.6f;
    glm::vec2 windVelocity{12.0f, 5.0f}; // m/s
    int cloudSteps = 40;
    int cloudLightSteps = 5;

    // --- Results (valid after prepare) ----------------------------------------
    glm::vec3 towardsSun() const { return m_sunDir; }
    glm::vec3 skyAmbient() const { return m_ambientSky; }
    // Bindless index of the tileable 3D noise (R = Perlin-Worley, GBA = Worley).
    uint32_t noiseTextureIndex() const;

    // Fraction of sunlight (per RGB channel) that survives the trip through the
    // atmosphere for a sun in direction `towardsSun`. ~white at noon, orange/red
    // at sunset, 0 below the horizon.
    static glm::vec3 sunTransmittance(const glm::vec3& towardsSun, float haze = 1.5f);
    // Sky radiance in direction `dir` (CPU version of the LUT shader; coarse).
    static glm::vec3 skyRadiance(const glm::vec3& dir, const glm::vec3& towardsSun, float haze, float mieG,
                                 float multiScatter);

private:
    ve::TextureId m_lut = ve::kNoTexture;
    ve::TextureId m_clouds = ve::kNoTexture;
    VkPipeline m_lutPipeline = VK_NULL_HANDLE;
    VkPipeline m_cloudPipeline = VK_NULL_HANDLE;
    VkPipeline m_skyPipeline = VK_NULL_HANDLE;
    std::shared_ptr<ve::Texture> m_noise;

    glm::vec3 m_sunDir{0.0f, 1.0f, 0.0f};
    glm::vec3 m_sunRadiance{1.0f};
    glm::vec3 m_ambientSky{0.0f};
    glm::vec2 m_windOffset{0.0f};
    float m_cameraHeight = 1.0f;
};

} // namespace vfx
