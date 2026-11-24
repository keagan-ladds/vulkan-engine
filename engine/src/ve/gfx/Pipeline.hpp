#pragma once

// =============================================================================
// Pipeline helpers: shader module loading, a fluent graphics PipelineBuilder
// (grown from tinyvulkan's) and a compute pipeline helper.
//
// Everything uses dynamic rendering (no VkRenderPass) and dynamic
// viewport/scissor (+ optional dynamic depth bias), so pipelines never need
// rebuilding on resize.
// =============================================================================

#include "ve/gfx/Vk.hpp"

#include <filesystem>
#include <span>
#include <vector>

namespace ve {

enum class BlendMode {
    Opaque,        ///< overwrite
    AlphaBlend,    ///< src*a + dst*(1-a)
    Additive,      ///< src*a + dst      (fire, sparks, glows)
    Premultiplied, ///< src + dst*(1-a)
    Multiply,      ///< src*dst
    /// Custom blend used by fog-like passes: dst = src.rgb + dst.rgb * src.a
    /// (src.rgb = in-scattered light, src.a = transmittance).
    InscatterTransmittance,
};

enum class CullMode { None, Back, Front };

VkShaderModule loadShaderModule(VkDevice device, const std::filesystem::path& spirvPath);

/// The VkPipelineCache every pipeline below is created with (owned by Device,
/// which sets it at init and clears it at shutdown). VK_NULL_HANDLE = none.
void setPipelineCache(VkPipelineCache cache);

/// Builds a graphics pipeline for dynamic rendering, one setting at a time:
///     VkPipeline p = PipelineBuilder().shaders(vs, fs).layout(layout)
///                        .colorAttachment(format).depthAttachment(depth).build(device);
class PipelineBuilder {
public:
    PipelineBuilder();

    PipelineBuilder& shaders(VkShaderModule vertex, VkShaderModule fragment); ///< fragment may be VK_NULL_HANDLE (depth-only)
    /// Vertex buffer layout (none: the shader makes its vertices, e.g. a full-screen triangle).
    PipelineBuilder& vertexInput(std::span<const VkVertexInputBindingDescription> bindings,
                                 std::span<const VkVertexInputAttributeDescription> attributes);
    PipelineBuilder& topology(VkPrimitiveTopology topology);  ///< Primitive type (default triangle list).
    PipelineBuilder& polygonMode(VkPolygonMode mode);         ///< Fill or lines (wireframe).
    PipelineBuilder& cull(CullMode mode, VkFrontFace front = VK_FRONT_FACE_COUNTER_CLOCKWISE); ///< Face culling; front faces are counter-clockwise by default.
    /// write = false: the attachment is bound but this pipeline doesn't touch it.
    PipelineBuilder& colorAttachment(VkFormat format, BlendMode blend = BlendMode::Opaque, bool write = true);
    PipelineBuilder& depthAttachment(VkFormat format);                  ///< The depth attachment's format.
    PipelineBuilder& depthTest(bool test, bool write, VkCompareOp op);  ///< Depth test / write and comparison.
    PipelineBuilder& dynamicDepthBias(bool enable);                     ///< Depth bias set per draw (shadow maps).
    PipelineBuilder& depthClamp(bool enable);                           ///< Clamp depth instead of clipping (needs the feature).
    PipelineBuilder& layout(VkPipelineLayout layout);                   ///< The pipeline layout (required).

    /// Creates the pipeline (with the shared pipeline cache); the caller destroys it.
    VkPipeline build(VkDevice device) const;

private:
    std::vector<VkPipelineShaderStageCreateInfo> m_stages;
    std::vector<VkVertexInputBindingDescription> m_bindings;
    std::vector<VkVertexInputAttributeDescription> m_attributes;
    VkPipelineInputAssemblyStateCreateInfo m_inputAssembly{};
    VkPipelineRasterizationStateCreateInfo m_raster{};
    VkPipelineDepthStencilStateCreateInfo m_depth{};
    std::vector<VkFormat> m_colorFormats;
    std::vector<VkPipelineColorBlendAttachmentState> m_blends;
    VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;
    bool m_dynamicDepthBias = false;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
};

VkPipeline createComputePipeline(VkDevice device, VkShaderModule shader, VkPipelineLayout layout);

} // namespace ve
