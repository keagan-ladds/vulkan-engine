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

class FullscreenPass : public RenderPass {
public:
    struct Desc {
        std::string fragment;                 ///< SPIR-V name, e.g. "composite.frag"
        std::string output = "SceneColor";   ///< graph texture to render into ("Backbuffer" for the swapchain)
        std::vector<std::string> inputs;      ///< graph textures sampled by the shader
        BlendMode blend = BlendMode::Opaque;
        bool clear = false;
        glm::vec4 clearColor{0.0f};
        /// Optional read-only depth test, e.g. {"SceneDepth", VK_COMPARE_OP_GREATER_OR_EQUAL}
        /// with fullscreen.vert at depth 0 => only pixels where nothing was drawn (sky).
        std::string depth;
        VkCompareOp depthCompare = VK_COMPARE_OP_GREATER_OR_EQUAL;
        bool samplesShadows = false;          ///< declare reads of the shadow maps
    };

    FullscreenPass(std::string name, RenderStage stage, int order, Desc desc);
    ~FullscreenPass() override;

    void setup(RenderGraph& graph) override;
    void declare(PassBuilder& builder) override;
    void execute(RenderContext& ctx) override;

    /// Bindless index of input `i` (as listed in Desc::inputs).
    uint32_t inputIndex(size_t i) const;
    TextureId inputTexture(size_t i) const { return m_inputs[i]; }

    template <class T>
    void setPush(const T& data)
    {
        static_assert(sizeof(T) <= kPushConstantBytes, "push constants are limited to 128 bytes");
        std::memcpy(m_push.data(), &data, sizeof(T));
        m_pushSize = sizeof(T);
    }

protected:
    Desc m_desc;
    std::vector<TextureId> m_inputs;
    TextureId m_output = kNoTexture;
    TextureId m_depth = kNoTexture;
    PipelineHandle m_pipeline;
    std::array<uint8_t, kPushConstantBytes> m_push{};
    uint32_t m_pushSize = 0;
};

} // namespace ve
