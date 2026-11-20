#include "ve/gfx/Pipeline.hpp"

#include <fstream>

namespace ve {

namespace {
VkPipelineCache g_pipelineCache = VK_NULL_HANDLE; // Device's (pipeline caches are internally synchronized)
} // namespace

void setPipelineCache(VkPipelineCache cache)
{
    g_pipelineCache = cache;
}

VkShaderModule loadShaderModule(VkDevice device, const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open shader: " + path.string());
    const auto bytes = static_cast<size_t>(file.tellg());
    if (bytes == 0 || bytes % 4 != 0) throw std::runtime_error("Invalid SPIR-V: " + path.string());
    std::vector<uint32_t> code(bytes / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(bytes));

    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = bytes;
    info.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(device, &info, nullptr, &module));
    return module;
}

PipelineBuilder::PipelineBuilder()
{
    m_inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    m_inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    m_raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    m_raster.polygonMode = VK_POLYGON_MODE_FILL;
    m_raster.cullMode = VK_CULL_MODE_NONE;
    m_raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    m_raster.lineWidth = 1.0f;

    m_depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    m_depth.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    m_depth.maxDepthBounds = 1.0f;
}

PipelineBuilder& PipelineBuilder::shaders(VkShaderModule vertex, VkShaderModule fragment)
{
    m_stages.clear();
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.pName = "main";
    stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    stage.module = vertex;
    m_stages.push_back(stage);
    if (fragment) {
        stage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stage.module = fragment;
        m_stages.push_back(stage);
    }
    return *this;
}

PipelineBuilder& PipelineBuilder::vertexInput(std::span<const VkVertexInputBindingDescription> bindings,
                                              std::span<const VkVertexInputAttributeDescription> attributes)
{
    m_bindings.assign(bindings.begin(), bindings.end());
    m_attributes.assign(attributes.begin(), attributes.end());
    return *this;
}

PipelineBuilder& PipelineBuilder::topology(VkPrimitiveTopology t)
{
    m_inputAssembly.topology = t;
    return *this;
}

PipelineBuilder& PipelineBuilder::polygonMode(VkPolygonMode mode)
{
    m_raster.polygonMode = mode;
    return *this;
}

PipelineBuilder& PipelineBuilder::cull(CullMode mode, VkFrontFace front)
{
    m_raster.cullMode = mode == CullMode::Back    ? VK_CULL_MODE_BACK_BIT
                        : mode == CullMode::Front ? VK_CULL_MODE_FRONT_BIT
                                                  : VK_CULL_MODE_NONE;
    m_raster.frontFace = front;
    return *this;
}

PipelineBuilder& PipelineBuilder::colorAttachment(VkFormat format, BlendMode blend, bool write)
{
    VkPipelineColorBlendAttachmentState s{};
    s.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                       VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    s.colorBlendOp = VK_BLEND_OP_ADD;
    s.alphaBlendOp = VK_BLEND_OP_ADD;
    switch (blend) {
    case BlendMode::Opaque:
        s.blendEnable = VK_FALSE;
        break;
    case BlendMode::AlphaBlend:
        s.blendEnable = VK_TRUE;
        s.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        s.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        s.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        s.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case BlendMode::Additive:
        s.blendEnable = VK_TRUE;
        s.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        s.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        s.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        s.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        break;
    case BlendMode::Premultiplied:
        s.blendEnable = VK_TRUE;
        s.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        s.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        s.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        s.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case BlendMode::Multiply:
        s.blendEnable = VK_TRUE;
        s.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
        s.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        s.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        s.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        break;
    case BlendMode::InscatterTransmittance:
        s.blendEnable = VK_TRUE;
        s.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        s.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        s.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        s.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        break;
    }
    if (!write) s.colorWriteMask = 0;
    m_colorFormats.push_back(format);
    m_blends.push_back(s);
    return *this;
}

PipelineBuilder& PipelineBuilder::depthAttachment(VkFormat format)
{
    m_depthFormat = format;
    return *this;
}

PipelineBuilder& PipelineBuilder::depthTest(bool test, bool write, VkCompareOp op)
{
    m_depth.depthTestEnable = test ? VK_TRUE : VK_FALSE;
    m_depth.depthWriteEnable = write ? VK_TRUE : VK_FALSE;
    m_depth.depthCompareOp = op;
    return *this;
}

PipelineBuilder& PipelineBuilder::dynamicDepthBias(bool enable)
{
    m_dynamicDepthBias = enable;
    m_raster.depthBiasEnable = enable ? VK_TRUE : VK_FALSE;
    return *this;
}

PipelineBuilder& PipelineBuilder::depthClamp(bool enable)
{
    m_raster.depthClampEnable = enable ? VK_TRUE : VK_FALSE;
    return *this;
}

PipelineBuilder& PipelineBuilder::layout(VkPipelineLayout l)
{
    m_layout = l;
    return *this;
}

VkPipeline PipelineBuilder::build(VkDevice device) const
{
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(m_bindings.size());
    vertexInput.pVertexBindingDescriptions = m_bindings.data();
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(m_attributes.size());
    vertexInput.pVertexAttributeDescriptions = m_attributes.data();

    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineMultisampleStateCreateInfo msaa{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    msaa.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    msaa.minSampleShading = 1.0f;

    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = static_cast<uint32_t>(m_blends.size());
    blend.pAttachments = m_blends.data();

    std::vector<VkDynamicState> dynamics = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    if (m_dynamicDepthBias) dynamics.push_back(VK_DYNAMIC_STATE_DEPTH_BIAS);
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = static_cast<uint32_t>(dynamics.size());
    dynamic.pDynamicStates = dynamics.data();

    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = static_cast<uint32_t>(m_colorFormats.size());
    rendering.pColorAttachmentFormats = m_colorFormats.data();
    rendering.depthAttachmentFormat = m_depthFormat;

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = static_cast<uint32_t>(m_stages.size());
    info.pStages = m_stages.data();
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &m_inputAssembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &m_raster;
    info.pMultisampleState = &msaa;
    info.pDepthStencilState = &m_depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = m_layout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(device, g_pipelineCache, 1, &info, nullptr, &pipeline));
    return pipeline;
}

VkPipeline createComputePipeline(VkDevice device, VkShaderModule shader, VkPipelineLayout layout)
{
    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = shader;
    info.stage.pName = "main";
    info.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VK_CHECK(vkCreateComputePipelines(device, g_pipelineCache, 1, &info, nullptr, &pipeline));
    return pipeline;
}

} // namespace ve
