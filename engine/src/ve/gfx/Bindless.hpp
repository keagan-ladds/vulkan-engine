#pragma once

// =============================================================================
// Bindless - one big descriptor set (set 1) holding every texture in the game.
//
// Classic Vulkan binds a descriptor set per material listing its textures.
// "Bindless" instead puts *all* textures in one huge array, bound once; a
// shader picks a texture with an integer index:
//
//     vec4 c = texture(veTextures[nonuniformEXT(idx)], uv);
//
// Materials, post-process passes, particles, ... just carry uint indices.
// This relies on Vulkan 1.2 descriptor indexing (UPDATE_AFTER_BIND +
// PARTIALLY_BOUND) so we can add textures while the set is in use.
//
//   binding 0: storage images [kMaxStorageImages]   (compute writes)
//   binding 1: combined image samplers [kMaxTextures]
// =============================================================================

#include "ve/gfx/Vk.hpp"

#include <cstdint>
#include <vector>

namespace ve {

class Device;

class Bindless {
public:
    static constexpr uint32_t kMaxTextures = 4096;
    static constexpr uint32_t kMaxStorageImages = 256;
    static constexpr uint32_t kInvalid = 0xFFFFFFFFu;

    void init(Device& device);
    void shutdown();

    VkDescriptorSetLayout layout() const { return m_layout; }
    VkDescriptorSet set() const { return m_set; }

    // Image views must be in VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL when sampled.
    uint32_t addTexture(VkImageView view, VkSampler sampler);
    void updateTexture(uint32_t index, VkImageView view, VkSampler sampler);
    // Freed slots are recycled kFramesInFlight frames later and point at the
    // fallback texture meanwhile.
    void removeTexture(uint32_t index);

    // Storage images must be in VK_IMAGE_LAYOUT_GENERAL when accessed.
    uint32_t addStorageImage(VkImageView view);
    void updateStorageImage(uint32_t index, VkImageView view);
    void removeStorageImage(uint32_t index);

    // Texture written into freed slots (set by the renderer once its default
    // white texture exists).
    void setFallback(VkImageView view, VkSampler sampler);

private:
    void writeTexture(uint32_t index, VkImageView view, VkSampler sampler);
    void writeStorage(uint32_t index, VkImageView view);

    Device* m_device = nullptr;
    VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;

    uint32_t m_nextTexture = 0;
    uint32_t m_nextStorage = 0;
    std::vector<uint32_t> m_freeTextures;
    std::vector<uint32_t> m_freeStorage;
    VkImageView m_fallbackView = VK_NULL_HANDLE;
    VkSampler m_fallbackSampler = VK_NULL_HANDLE;
};

} // namespace ve
