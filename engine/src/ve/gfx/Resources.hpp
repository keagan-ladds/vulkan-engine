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

// Where a buffer's memory should live.
enum class MemoryUsage {
    GpuOnly,  // device-local; fastest for the GPU, filled via staging uploads
    CpuToGpu, // host-visible & persistently mapped; for per-frame data (UBOs, instance data)
    GpuToCpu, // host-visible, cached; for readbacks
};

struct Buffer {
    VkBuffer handle = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VkDeviceSize size = 0;
    void* mapped = nullptr; // non-null for CpuToGpu / GpuToCpu buffers
};

struct ImageDesc {
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkExtent3D extent{1, 1, 1};
    uint32_t mipLevels = 1;
    uint32_t layers = 1;                         // array layers (cube = 6, cube array = 6*n)
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D; // 2D, 2D_ARRAY, 3D, CUBE, CUBE_ARRAY
    bool perLayerViews = false;                  // also create one 2D view per layer (render targets)
    const char* debugName = nullptr;
};

struct Image {
    VkImage handle = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VkImageView view = VK_NULL_HANDLE;           // view of the whole image (all mips/layers)
    std::vector<VkImageView> layerViews;         // optional: one 2D view per layer
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent3D extent{0, 0, 0};
    uint32_t mipLevels = 1;
    uint32_t layers = 1;
    VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
};

} // namespace ve
