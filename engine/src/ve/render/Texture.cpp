#include "ve/render/Texture.hpp"

#include "ve/gfx/Bindless.hpp"
#include "ve/gfx/Device.hpp"
#include "ve/render/Renderer.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace ve {

std::shared_ptr<Texture> Texture::createImpl(Renderer& renderer, const ImageDesc& desc, const void* pixels,
                                             size_t byteSize, bool mipmaps, SamplerType sampler, std::string name)
{
    std::shared_ptr<Texture> tex(new Texture());
    tex->m_renderer = &renderer;
    tex->m_name = std::move(name);
    ImageDesc d = desc;
    d.debugName = tex->m_name.empty() ? nullptr : tex->m_name.c_str();
    tex->m_image = renderer.device().createImage(d);
    renderer.device().uploadImage(tex->m_image, pixels, byteSize, mipmaps);
    tex->m_index = renderer.bindless().addTexture(tex->m_image.view, renderer.samplers().get(sampler));
    return tex;
}

std::shared_ptr<Texture> Texture::create2D(Renderer& renderer, uint32_t width, uint32_t height, VkFormat format,
                                           const void* pixels, size_t byteSize, bool mipmaps, SamplerType sampler,
                                           std::string name)
{
    ImageDesc desc;
    desc.format = format;
    desc.extent = {width, height, 1};
    // Full chain: floor(log2(max(w,h))) + 1 levels, e.g. 1024 -> 11.
    desc.mipLevels = mipmaps ? static_cast<uint32_t>(std::bit_width(std::max(width, height))) : 1;
    desc.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    desc.viewType = VK_IMAGE_VIEW_TYPE_2D;
    return createImpl(renderer, desc, pixels, byteSize, mipmaps, sampler, std::move(name));
}

std::shared_ptr<Texture> Texture::create3D(Renderer& renderer, uint32_t width, uint32_t height, uint32_t depth,
                                           VkFormat format, const void* pixels, size_t byteSize, SamplerType sampler,
                                           std::string name)
{
    ImageDesc desc;
    desc.format = format;
    desc.extent = {width, height, depth};
    desc.mipLevels = 1;
    desc.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    desc.viewType = VK_IMAGE_VIEW_TYPE_3D;
    return createImpl(renderer, desc, pixels, byteSize, false, sampler, std::move(name));
}

std::shared_ptr<Texture> Texture::adopt(Renderer& renderer, const Image& image, SamplerType sampler, std::string name)
{
    std::shared_ptr<Texture> tex(new Texture());
    tex->m_renderer = &renderer;
    tex->m_name = std::move(name);
    tex->m_image = image;
    tex->m_index = renderer.bindless().addTexture(image.view, renderer.samplers().get(sampler));
    return tex;
}

std::shared_ptr<Texture> Texture::solid(Renderer& renderer, uint8_t r, uint8_t g, uint8_t b, uint8_t a, bool srgb,
                                        std::string name)
{
    const uint8_t px[4] = {r, g, b, a};
    return create2D(renderer, 1, 1, srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, px, 4, false,
                    SamplerType::LinearRepeat, std::move(name));
}

void Texture::setSampler(SamplerType sampler)
{
    m_renderer->bindless().updateTexture(m_index, m_image.view, m_renderer->samplers().get(sampler));
}

Texture::~Texture()
{
    Device* device = &m_renderer->device();
    m_renderer->bindless().removeTexture(m_index);
    Image img = m_image;
    device->deferDestroy([device, img]() mutable { device->destroyImage(img); });
}

} // namespace ve
