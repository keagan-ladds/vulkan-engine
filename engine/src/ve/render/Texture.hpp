#pragma once

// =============================================================================
// Texture - a sampled GPU image that is automatically registered in the
// bindless table. Shaders use texture->index() to sample it:
//
//     auto tex = Texture::create2D(renderer, w, h, VK_FORMAT_R8G8B8A8_SRGB, pixels);
//     material->setTexture(0, tex);   // stored as tex->index() in the material
//
// Loading from files lives in AssetManager (assets().loadTexture("x.png")).
// =============================================================================

#include "ve/gfx/Resources.hpp"
#include "ve/gfx/Samplers.hpp"

#include <memory>
#include <span>
#include <string>

namespace ve {

class Renderer;

class Texture {
public:
    /// `pixels` must be tightly packed. mipmaps = generate a full mip chain.
    static std::shared_ptr<Texture> create2D(Renderer& renderer, uint32_t width, uint32_t height, VkFormat format,
                                             const void* pixels, size_t byteSize, bool mipmaps = true,
                                             SamplerType sampler = SamplerType::LinearRepeat, std::string name = {});
    /// 3D volume texture (e.g. cloud noise). No mips.
    static std::shared_ptr<Texture> create3D(Renderer& renderer, uint32_t width, uint32_t height, uint32_t depth,
                                             VkFormat format, const void* pixels, size_t byteSize,
                                             SamplerType sampler = SamplerType::LinearRepeat, std::string name = {});
    /// Takes ownership of an image that's already in READ_ONLY_OPTIMAL layout
    /// (e.g. rendered on the GPU, like impostor atlases).
    static std::shared_ptr<Texture> adopt(Renderer& renderer, const Image& image, SamplerType sampler,
                                          std::string name = {});
    /// A 2D texture with a prebuilt mip chain (e.g. a cooked, block-compressed
    /// one): `data` holds every level, level i starting at levelOffsets[i].
    static std::shared_ptr<Texture> createWithLevels(Renderer& renderer, uint32_t width, uint32_t height, VkFormat format,
                                                     const void* data, size_t byteSize, std::span<const VkDeviceSize> levelOffsets,
                                                     SamplerType sampler, VkComponentMapping swizzle = {}, std::string name = {});
    /// Single-colour 1x1 texture.
    static std::shared_ptr<Texture> solid(Renderer& renderer, uint8_t r, uint8_t g, uint8_t b, uint8_t a,
                                          bool srgb, std::string name = {});
    ~Texture();

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    uint32_t index() const { return m_index; } ///< bindless slot
    const Image& image() const { return m_image; }
    uint32_t width() const { return m_image.extent.width; }
    uint32_t height() const { return m_image.extent.height; }
    const std::string& name() const { return m_name; }

    /// Change filtering/wrapping after creation.
    void setSampler(SamplerType sampler);

private:
    Texture() = default;
    static std::shared_ptr<Texture> createImpl(Renderer& renderer, const ImageDesc& desc, const void* pixels,
                                               size_t byteSize, bool mipmaps, SamplerType sampler, std::string name);

    Renderer* m_renderer = nullptr;
    Image m_image;
    uint32_t m_index = 0xFFFFFFFFu;
    std::string m_name;
};

} // namespace ve
