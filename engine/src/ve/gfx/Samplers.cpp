#include "ve/gfx/Samplers.hpp"

#include "ve/gfx/Device.hpp"

#include <algorithm>

namespace ve {

void Samplers::init(const Device& device)
{
    auto make = [&](VkFilter filter, VkSamplerAddressMode address, bool anisotropy, bool compare) {
        VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        info.magFilter = filter;
        info.minFilter = filter;
        info.mipmapMode = filter == VK_FILTER_LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU = info.addressModeV = info.addressModeW = address;
        info.maxLod = VK_LOD_CLAMP_NONE;
        if (anisotropy && device.features().samplerAnisotropy) {
            info.anisotropyEnable = VK_TRUE;
            info.maxAnisotropy = std::min(8.0f, device.features().maxAnisotropy);
        }
        if (compare) {
            // Shadow maps use standard depth: lit if fragmentDepth <= storedDepth.
            info.compareEnable = VK_TRUE;
            info.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
            info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE; // outside the map = lit
            info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            info.maxLod = 0.0f;
        }
        VkSampler s = VK_NULL_HANDLE;
        VK_CHECK(vkCreateSampler(device.handle(), &info, nullptr, &s));
        return s;
    };

    m_samplers[size_t(SamplerType::LinearRepeat)] = make(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT, true, false);
    m_samplers[size_t(SamplerType::LinearClamp)] = make(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, false, false);
    m_samplers[size_t(SamplerType::NearestRepeat)] = make(VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_REPEAT, false, false);
    m_samplers[size_t(SamplerType::NearestClamp)] = make(VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, false, false);
    m_samplers[size_t(SamplerType::ShadowCompare)] = make(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, false, true);
}

void Samplers::shutdown(const Device& device)
{
    for (VkSampler& s : m_samplers) {
        if (s) vkDestroySampler(device.handle(), s, nullptr);
        s = VK_NULL_HANDLE;
    }
}

} // namespace ve
