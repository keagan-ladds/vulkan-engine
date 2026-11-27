#pragma once

// =============================================================================
// FullscreenPass - base class for "draw one triangle covering the screen with
// a fragment shader" passes: post-processing, sky, fog, tonemapping...
//
// The fragment shader includes ve.glsl, so it can read every frame global,
// light and shadow map. Input textures are passed as bindless indices in
// push constants (the shader must #define VE_CUSTOM_PUSH_CONSTANTS and
// declare its own push block). Minimal example:
//
//   struct VignettePush { uint32_t sceneColor; float strength; };
//   class VignettePass : public ve::FullscreenPass {
//   public:
//       VignettePass() : FullscreenPass("Vignette", RenderStage::PostProcess, 0,
//                        {.fragment = "vignette.frag", .output = "SceneColor", .inputs = {"SceneColor"}}) {}
//       void prepare(FrameData&) override {
//           setPush(VignettePush{inputIndex(0), 0.4f});
//       }
//   };
//
// NOTE: reading and writing the same texture (output listed in inputs) is
// not allowed by Vulkan; copy first or render to a separate target. The
// example above should really use a temp texture; see vfx::BloomPass.
// =============================================================================

#include "ve/gfx/Pipeline.hpp"
#include "ve/render/PipelineHandle.hpp"
#include "ve/render/RenderGraph.hpp"

#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace ve {

/// Base class for full-screen fragment-shader passes (see the top of this file).
class FullscreenPass : public RenderPass {
public:
    /// What the pass reads and writes, and its pipeline state.
    struct Desc {
        std::string fragment;                 ///< SPIR-V name, e.g. "composite.frag"
        std::string output = "SceneColor";   ///< graph texture to render into ("Backbuffer" for the swapchain)
        std::vector<std::string> inputs;      ///< graph textures sampled by the shader
        BlendMode blend = BlendMode::Opaque;  ///< How the output is blended over the target.
        bool clear = false;                   ///< Clear the output first.
        glm::vec4 clearColor{0.0f};           ///< Clear value.
        /// Optional read-only depth test, e.g. {"SceneDepth", VK_COMPARE_OP_GREATER_OR_EQUAL}
        /// with fullscreen.vert at depth 0 => only pixels where nothing was drawn (sky).
        std::string depth;
        VkCompareOp depthCompare = VK_COMPARE_OP_GREATER_OR_EQUAL; ///< Depth test against `depth` (reversed Z).
        bool samplesShadows = false;          ///< declare reads of the shadow maps
    };

    /// A pass called `name` at `stage` / `order` (see RenderPass) drawing `desc`.
    FullscreenPass(std::string name, RenderStage stage, int order, Desc desc);
    ~FullscreenPass() override;

    void setup(RenderGraph& graph) override;
    void declare(PassBuilder& builder) override;
    void execute(RenderContext& ctx) override;

    /// Bindless index of input `i` (as listed in Desc::inputs).
    uint32_t inputIndex(size_t i) const;
    TextureId inputTexture(size_t i) const { return m_inputs[i]; } ///< Graph texture of input `i`.

    /// Sets the push constants the shader gets (call in prepare(); at most kPushConstantBytes).
    template <class T>
    void setPush(const T& data)
    {
        static_assert(sizeof(T) <= kPushConstantBytes, "push constants are limited to 128 bytes");
        std::memcpy(m_push.data(), &data, sizeof(T));
        m_pushSize = sizeof(T);
    }

protected:
    Desc m_desc;                                       ///< As given.
    std::vector<TextureId> m_inputs;                   ///< Resolved inputs.
    TextureId m_output = kNoTexture;                   ///< Resolved output.
    TextureId m_depth = kNoTexture;                    ///< Resolved depth (kNoTexture = none).
    PipelineHandle m_pipeline;                         ///< The pass's pipeline.
    std::array<uint8_t, kPushConstantBytes> m_push{};  ///< Push constants set by setPush.
    uint32_t m_pushSize = 0;                           ///< Bytes used in m_push.
};

} // namespace ve
