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
    Opaque,        // overwrite
    AlphaBlend,    // src*a + dst*(1-a)
    Additive,      // src*a + dst      (fire, sparks, glows)
    Premultiplied, // src + dst*(1-a)
    Multiply,      // src*dst
    // Custom blend used by fog-like passes: dst = src.rgb + dst.rgb * src.a
    // (src.rgb = in-scattered light, src.a = transmittance).
    InscatterTransmittance,
};

enum class CullMode { None, Back, Front };

VkShaderModule loadShaderModule(VkDevice device, const std::filesystem::path& spirvPath);

class PipelineBuilder {
public:
    PipelineBuilder();

    PipelineBuilder& shaders(VkShaderModule vertex, VkShaderModule fragment); // fragment may be VK_NULL_HANDLE (depth-only)
    PipelineBuilder& vertexInput(std::span<const VkVertexInputBindingDescription> bindings,
                                 std::span<const VkVertexInputAttributeDescription> attributes);
    PipelineBuilder& topology(VkPrimitiveTopology topology);
    PipelineBuilder& polygonMode(VkPolygonMode mode);
    PipelineBuilder& cull(CullMode mode, VkFrontFace front = VK_FRONT_FACE_COUNTER_CLOCKWISE);
    // write = false: the attachment is bound but this pipeline doesn't touch it.
    PipelineBuilder& colorAttachment(VkFormat format, BlendMode blend = BlendMode::Opaque, bool write = true);
    PipelineBuilder& depthAttachment(VkFormat format);
    PipelineBuilder& depthTest(bool test, bool write, VkCompareOp op);
    PipelineBuilder& dynamicDepthBias(bool enable);
    PipelineBuilder& depthClamp(bool enable);
    PipelineBuilder& layout(VkPipelineLayout layout);

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
