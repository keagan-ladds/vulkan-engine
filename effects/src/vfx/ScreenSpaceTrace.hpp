#pragma once

// =============================================================================
// ScreenSpaceTracePass - the shared shape of ScreenSpaceGIPass and
// ScreenSpaceReflectionsPass:
//   1. trace: a full-screen shader at reduced resolution reads SceneDepth,
//      SceneNormal and the lit SceneColor and writes <Name>Buffer;
//   2. TemporalAccumulator blends it with the reprojected history;
//   3. apply: a full-screen shader upsamples the result and blends it over
//      SceneColor.
// Subclasses provide the shader names, blend mode and push constants.
// =============================================================================

#include "vfx/TemporalAccumulator.hpp"

#include <ve/render/PipelineHandle.hpp>
#include <ve/render/RenderGraph.hpp>
#include <ve/render/Renderer.hpp>

#include <string>

namespace vfx {

/// Base of screen-space ray-traced effects (SSR, SSGI): a trace pass at reduced resolution, temporal accumulation, and an apply pass over SceneColor.
class ScreenSpaceTracePass : public ve::RenderPass {
public:
    ~ScreenSpaceTracePass() override;

    void setup(ve::RenderGraph& graph) override;
    void declare(ve::PassBuilder& builder) override;
    void execute(ve::RenderContext& ctx) override;
    void onResize(ve::RenderGraph&) override { temporal.invalidate(); }

    TemporalAccumulator temporal; ///< accumulates over frames (much less noise); temporal.enabled = false to disable

protected:
    /// The shaders and blending of a derived pass.
    struct Shaders {
        std::string prefix;      ///< "SSGI" -> SSGIBuffer + the accumulator's textures
        std::string trace;       ///< fragment shaders
        std::string apply;  ///< Fragment shader that applies the result to SceneColor.
        ve::BlendMode applyBlend;  ///< How the apply pass blends.
    };
    /// @param resolutionScale trace resolution relative to the screen (0.5 = half).
    ScreenSpaceTracePass(std::string name, int order, float resolutionScale, Shaders shaders);

    /// Inputs for the shaders (bindless indices).
    struct Inputs {
        uint32_t depth;   ///< Scene depth.
        uint32_t normal;  ///< Thin G-buffer (normal + roughness).
        uint32_t color;   ///< Scene colour.
    };
    /// Push constants for the trace, and for the apply (`result` = the
    /// accumulated buffer, `size` = its size in texels).
    virtual void pushTrace(ve::RenderContext& ctx, const Inputs& in) = 0;
    /// Sets the push constants of the apply pass (`result` = the accumulated trace's bindless index).
    virtual void pushApply(ve::RenderContext& ctx, const Inputs& in, uint32_t result, glm::vec2 size) = 0;

private:
    float m_scale;
    Shaders m_shaders;
    ve::TextureId m_buffer = ve::kNoTexture;
    ve::PipelineHandle m_trace;
    ve::PipelineHandle m_apply;
};

} // namespace vfx
