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

#include <ve/render/PipelineHandle.hpp>
#include <ve/render/RenderGraph.hpp>
#include <ve/scene/ObjectRef.hpp>

#include <glm/glm.hpp>

#include <memory>

namespace ve {
class DirectionalLight;
class Texture;
} // namespace ve

namespace vfx {

/// The sky: atmosphere scattering, sun / moon, stars and volumetric clouds (see the top of this file).
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
    /// By GameObject id (looked up every frame, so deleting the light's object
    /// can't leave a dangling pointer). Empty / missing: no sun / moon.
    ve::ObjectRef<> sunObject;  ///< its DirectionalLight is the sun; else the brightest directional light
    ve::ObjectRef<> moonObject; ///< optional, its DirectionalLight draws a moon disc
    void setSun(const ve::DirectionalLight* light);   ///< Sets sunObject from a light (null = none).
    void setMoon(const ve::DirectionalLight* light);  ///< Sets moonObject from a light (null = none).

    // --- Sky -------------------------------------------------------------------
    float sunIlluminance = 9.0f;  ///< overall sky brightness
    float haze = 1.5f;            ///< aerosol (Mie) amount: >1 = hazy, dusty air
    float mieAnisotropy = 0.8f;   ///< size of the glow around the sun
    float multipleScattering = 1.0f; ///< Approximate multiple scattering: brightens the sky's base colour.
    float sunDiskSize = 1.0f;     ///< multiplier on the real 0.27 degree radius
    float sunDiskBrightness = 40.0f; ///< Multiplier on the sun disc's radiance.
    float starBrightness = 1.0f;     ///< Night stars (0 = none).
    float overcast = 0.0f;        ///< 0..1: blocks direct sun, greys the sky (set by Weather)
    glm::vec3 groundAlbedo{0.3f, 0.25f, 0.18f}; ///< Ground colour bouncing light back into the sky / ambient.

    // --- Ambient ---------------------------------------------------------------
    bool driveAmbient = true;     ///< set renderer ambient from the sky each frame
    float ambientScale = 1.0f;    ///< Multiplier on the ambient derived from the sky.
    glm::vec3 nightAmbient{0.010f, 0.014f, 0.028f}; ///< Minimum ambient at night (linear RGB, moonlit sky).

    // --- Clouds ----------------------------------------------------------------
    bool clouds = true;           ///< Draw volumetric clouds.
    float cloudBottom = 1400.0f;  ///< meters
    float cloudTop = 3200.0f;     ///< meters
    float coverage = 0.45f;       ///< 0 = clear, 1 = overcast
    float cloudDensity = 1.0f;    ///< Cloud opacity multiplier.
    float baseScale = 9000.0f;    ///< meters per noise tile (cloud size)
    float detailScale = 1700.0f;  ///< Meters per detail noise tile (erodes the cloud edges).
    float detailStrength = 0.35f; ///< How much the detail noise erodes (0..1).
    float cloudForwardScattering = 0.6f; ///< Silver lining towards the sun (phase anisotropy 0..1).
    glm::vec2 windVelocity{12.0f, 5.0f}; ///< m/s
    int cloudSteps = 40;          ///< Raymarch steps through the cloud layer (quality vs speed).
    int cloudLightSteps = 5;      ///< Steps towards the sun per sample (cloud self-shadowing).

    // --- Results (valid after prepare) ----------------------------------------
    glm::vec3 towardsSun() const { return m_sunDir; }       ///< Unit direction towards the sun.
    glm::vec3 skyAmbient() const { return m_ambientSky; }   ///< The ambient light derived from the sky.
    /// Bindless index of the tileable 3D noise (R = Perlin-Worley, GBA = Worley).
    uint32_t noiseTextureIndex() const;

    /// Fraction of sunlight (per RGB channel) that survives the trip through the
    /// atmosphere for a sun in direction `towardsSun`. ~white at noon, orange/red
    /// at sunset, 0 below the horizon.
    static glm::vec3 sunTransmittance(const glm::vec3& towardsSun, float haze = 1.5f);
    /// Sky radiance in direction `dir` (CPU version of the LUT shader; coarse).
    static glm::vec3 skyRadiance(const glm::vec3& dir, const glm::vec3& towardsSun, float haze, float mieG,
                                 float multiScatter);

private:
    ve::TextureId m_lut = ve::kNoTexture;
    ve::TextureId m_clouds = ve::kNoTexture;
    ve::PipelineHandle m_lutPipeline;
    ve::PipelineHandle m_cloudPipeline;
    ve::PipelineHandle m_skyPipeline;
    std::shared_ptr<ve::Texture> m_noise;

    /// sunObject / moonObject resolved in prepare(); valid for the current frame only.
    ve::DirectionalLight* m_sun = nullptr;
    ve::DirectionalLight* m_moon = nullptr;
    glm::vec3 m_sunDir{0.0f, 1.0f, 0.0f};
    glm::vec3 m_sunRadiance{1.0f};
    glm::vec3 m_ambientSky{0.0f};
    glm::vec2 m_windOffset{0.0f};
    float m_cameraHeight = 1.0f;
};

} // namespace vfx
