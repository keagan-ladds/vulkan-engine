#pragma once

#include <cmath>

// =============================================================================
// The engine's built-in render passes (always present, can be disabled):
//
//   ShadowPass       Shadow      directional cascades + point light cubes
//   OpaquePass       Opaque      clears SceneColor/SceneDepth, draws opaque + alpha-test queues
//   TransparentPass  Transparent draws the transparent queue back to front
//   VelocityPass     AfterOpaque motion vectors of moving objects (for TAA)
//   TemporalAAPass   PostProcess temporal anti-aliasing (RendererSettings::temporalAA), before other post effects
//   CompositePass    Composite   exposure + tonemapping + simple grading -> Backbuffer
//
// Effects (sky, fog, bloom, particles) live in the optional ve_effects module
// and slot in between using RenderStage.
// =============================================================================

#include "ve/render/passes/FullscreenPass.hpp"

namespace ve {

/// GPU skinning of every active SkinnedMeshRenderer (first thing in the frame).
class SkinningPass : public RenderPass {
public:
    SkinningPass() : RenderPass("Skinning", RenderStage::Shadow, -200) { runsWhileLoading = true; }
    void declare(PassBuilder&) override {}
    void execute(RenderContext& ctx) override;
};

/// Uploads changed instances and runs GPU culling for every InstanceGroup
/// (camera + directional shadow cascades) before anything is drawn.
class InstanceCullPass : public RenderPass {
public:
    InstanceCullPass() : RenderPass("Instance culling", RenderStage::Shadow, -100) { runsWhileLoading = true; } ///< instance uploads
    void declare(PassBuilder& builder) override; ///< last frame's depth pyramid, if occlusion culling
    void execute(RenderContext& ctx) override;
    void onGui() override;
};

/// Renders directional cascades and point-light cube faces (caching unchanged faces).
class ShadowPass : public RenderPass {
public:
    ShadowPass() : RenderPass("Shadows", RenderStage::Shadow) {}
    void declare(PassBuilder& builder) override;
    void execute(RenderContext& ctx) override;
    void onGui() override;

private:
    uint64_t materialHash(uint32_t gpuIndex); ///< of its GpuMaterial bytes, cached for this frame

    std::vector<uint32_t> m_casters;      ///< this frame's shadow-casting items
    std::vector<uint32_t> m_lightCasters; ///< the current point light's casters (in range)
    std::vector<uint32_t> m_scratch;      ///< the current view's casters
    std::vector<std::pair<uint64_t, uint64_t>> m_materialHashes; ///< per material: {frame stamp, hash}
    uint64_t m_materialHashStamp = 0;
    std::vector<uint64_t> m_pointFaceHash; ///< per point-shadow layer: inputs of its last render (0 = none)
    uint32_t m_pointFacesRendered = 0, m_pointFacesCached = 0;
};

/// Draws opaque geometry (meshes, instances) into SceneColor, the thin G-buffer and depth.
class OpaquePass : public RenderPass {
public:
    OpaquePass() : RenderPass("Opaque", RenderStage::Opaque) {}
    void declare(PassBuilder& builder) override;
    void execute(RenderContext& ctx) override;
    void onGui() override;
    glm::vec4 clearColor{0.0f, 0.0f, 0.0f, 1.0f}; ///< Background where nothing (not even sky) draws.
};

/// Draws transparent items back to front over the lit scene.
class TransparentPass : public RenderPass {
public:
    TransparentPass() : RenderPass("Transparent", RenderStage::Transparent) {}
    void declare(PassBuilder& builder) override;
    void execute(RenderContext& ctx) override;
};

/// Depth pyramid for occlusion culling (RendererSettings::occlusionCulling):
/// after the opaque geometry, SceneDepth is reduced to 512x256 .. 1x1 levels
/// holding the farthest depth of their area. Next frame, InstanceCullPass tests
/// each instance's bounding box (projected with last frame's camera) against
/// the level where it covers at most 2x2 texels: if all of it lies behind that
/// depth, it is hidden.
class HiZPass : public RenderPass {
public:
    HiZPass() : RenderPass("Hi-Z pyramid", RenderStage::AfterOpaque, -900) {}
    ~HiZPass() override;
    void setup(RenderGraph& graph) override;
    void declare(PassBuilder& builder) override;
    void execute(RenderContext& ctx) override;
    /// The pyramid built last frame (for this frame's cull), or null.
    const std::array<uint32_t, kHiZLevels>* lastFrame(uint64_t frameNumber) const;
    const std::array<TextureId, kHiZLevels>& textures() const { return m_levels; } ///< The pyramid's graph textures.

private:
    std::array<TextureId, kHiZLevels> m_levels{};
    std::array<uint32_t, kHiZLevels> m_indices{};
    PipelineHandle m_pipeline;
    uint64_t m_builtFrame = ~0ull;
    bool m_active = false;
};

/// Motion vectors for objects that moved since last frame (their world matrix
/// changed): each is drawn again into Velocity (RG16F, where it was on screen
/// last frame minus where it is now, in UV), where it is the visible surface.
/// Everything else keeps the cleared value (> 10 = "not moving"), for which
/// TAA derives the motion from depth and the camera. Vertex animation (wind,
/// skinning) isn't included. Only runs with TAA.
class VelocityPass : public RenderPass {
public:
    VelocityPass() : RenderPass("Motion vectors", RenderStage::AfterOpaque, -1000) {}
    ~VelocityPass() override;
    void setup(RenderGraph& graph) override;
    void declare(PassBuilder& builder) override;
    void execute(RenderContext& ctx) override;
    TextureId velocity() const { return m_velocity; } ///< The motion-vector texture.

private:
    TextureId m_velocity = kNoTexture;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    bool m_active = false;
};

/// Temporal anti-aliasing: blends the (jittered) frame into a history
/// reprojected with the camera motion (from depth), clipped to the current
/// 3x3 neighbourhood so moving objects don't leave trails. Only runs while
/// RendererSettings::temporalAA is on (which also jitters the camera).
class TemporalAAPass : public RenderPass {
public:
    TemporalAAPass() : RenderPass("Temporal AA", RenderStage::PostProcess, -100) {}
    ~TemporalAAPass() override;
    void setup(RenderGraph& graph) override;
    void declare(PassBuilder& builder) override;
    void execute(RenderContext& ctx) override;
    void onGui() override;

    float feedback = 0.9f; ///< history weight (higher = smoother, more ghosting)

private:
    TextureId m_history[2] = {kNoTexture, kNoTexture};
    PipelineHandle m_pipeline;
    uint32_t m_frame = 0;
    VkExtent3D m_extent{};
    uint64_t m_lastFrame = 0; ///< frame number of the last resolve (history is stale after a gap)
    bool m_active = false;
};

/// HDR -> display. Settings are public so games can drive them (e.g. dim at night).
class CompositePass : public FullscreenPass {
public:
    CompositePass();
    void prepare(FrameData& frame) override;
    void declare(PassBuilder& builder) override;
    void execute(RenderContext& ctx) override;
    void onGui() override;

    /// A grading pass (e.g. vfx::ColorGradingPass) that already applied exposure,
    /// grading and tonemapping calls this from its prepare() every frame; the
    /// composite then only adds vignette + dither. Cleared after each frame, so
    /// disabling the grading pass falls back to the built-in tonemapping.
    void useGradedInput(TextureId texture) { m_graded = texture; }
    /// exposure * exposureMultiplier * 2^exposureCompensation.
    float effectiveExposure() const { return exposure * exposureMultiplier * std::exp2(exposureCompensation); }

    float exposure = 1.0f;           ///< Linear exposure multiplier.
    float exposureMultiplier = 1.0f; ///< transient boosts (lightning flashes); multiplies exposure
    float exposureCompensation = 0.0f; ///< EV stops on top of everything (photo mode, brightness setting)
    float contrast = 1.05f;    ///< 1 = unchanged.
    float saturation = 1.1f;   ///< 1 = unchanged, 0 = grey.
    float vignette = 0.25f;    ///< Corner darkening (0 = none).
    glm::vec3 tint{1.0f}; ///< colour balance multiplier
    int tonemapper = 0;   ///< 0 = ACES (fitted), 1 = Reinhard, 2 = none

private:
    TextureId m_graded = kNoTexture;
};

} // namespace ve
