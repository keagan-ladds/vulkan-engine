#pragma once

// =============================================================================
// Plain GPU resource structs. They are created/destroyed by ve::Device
// (Device::createBuffer / createImage / destroy...). They are deliberately
// dumb data so they can be stored anywhere and copied around; ownership is
// handled by the higher-level classes (Mesh, Texture, RenderGraph).
// =============================================================================

#include "ve/gfx/Vk.hpp"

#include <vector>

namespace ve {

/// Where a buffer's memory should live.
enum class MemoryUsage {
    GpuOnly,  ///< device-local; fastest for the GPU, filled via staging uploads
    CpuToGpu, ///< host-visible & persistently mapped; for per-frame data (UBOs, instance data)
    GpuToCpu, ///< host-visible, cached; for readbacks
};

/// A GPU buffer and its memory (Device::createBuffer / destroyBuffer).
struct Buffer {
    VkBuffer handle = VK_NULL_HANDLE;  ///< The Vulkan buffer.
    VmaAllocation allocation = nullptr;  ///< Its VMA allocation.
    VkDeviceSize size = 0;  ///< Size in bytes.
    void* mapped = nullptr; ///< non-null for CpuToGpu / GpuToCpu buffers
};

/// How to create an image (Device::createImage).
struct ImageDesc {
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;  ///< Pixel format.
    VkExtent3D extent{1, 1, 1};  ///< Size in pixels (depth 1 except 3D images).
    uint32_t mipLevels = 1;  ///< Mip levels.
    uint32_t layers = 1;                         ///< array layers (cube = 6, cube array = 6*n)
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT;  ///< What it's used for (sampled, render target, storage, transfer...).
    VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D; ///< 2D, 2D_ARRAY, 3D, CUBE, CUBE_ARRAY
    bool perLayerViews = false;                  ///< also create one 2D view per layer (render targets)
    VkComponentMapping swizzle{};                ///< of the whole-image view (identity by default), e.g. R,R,R,1 for BC4
    const char* debugName = nullptr;  ///< Shown in validation messages and GPU debuggers.
};

/// A GPU image, its memory and views (Device::createImage / destroyImage).
struct Image {
    VkImage handle = VK_NULL_HANDLE;  ///< The Vulkan image.
    VmaAllocation allocation = nullptr;  ///< Its VMA allocation.
    VkImageView view = VK_NULL_HANDLE;           ///< view of the whole image (all mips/layers)
    std::vector<VkImageView> layerViews;         ///< optional: one 2D view per layer
    VkFormat format = VK_FORMAT_UNDEFINED;  ///< Pixel format.
    VkExtent3D extent{0, 0, 0};  ///< Size in pixels.
    uint32_t mipLevels = 1;  ///< Mip levels.
    uint32_t layers = 1;  ///< Array layers.
    VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;  ///< Type of `view`.
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;  ///< Colour or depth aspect.
};

} // namespace ve
