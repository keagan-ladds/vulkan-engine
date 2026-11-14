#pragma once

// =============================================================================
// Device - owns the long-lived Vulkan objects and provides resource helpers.
//
//   instance, debug messenger, surface, physical device, logical device,
//   graphics(+present) queue, VMA allocator, immediate-submit context,
//   deferred deletion queue.
//
// Deferred deletion: the CPU runs up to kFramesInFlight frames ahead of the
// GPU, so a resource released on the CPU may still be in use by the GPU.
// deferDestroy() queues the destruction and runs it kFramesInFlight frames
// later, when it's guaranteed to be finished with.
// =============================================================================

#include "ve/gfx/Resources.hpp"

#include <cstdint>
#include <deque>
#include <functional>

namespace ve {

class Window;

struct DeviceFeatures {
    bool samplerAnisotropy = false;
    float maxAnisotropy = 1.0f;
    bool depthClamp = false;
    bool fillModeNonSolid = false;
};

class Device {
public:
    static constexpr uint32_t kFramesInFlight = 2;

    Device() = default;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    void init(const Window& window, bool enableValidation);
    void shutdown();

    // --- Handles ---------------------------------------------------------------
    VkInstance instance() const { return m_instance; }
    VkPhysicalDevice physicalDevice() const { return m_physicalDevice; }
    VkDevice handle() const { return m_device; }
    VkSurfaceKHR surface() const { return m_surface; }
    VkQueue graphicsQueue() const { return m_graphicsQueue; }
    uint32_t graphicsFamily() const { return m_graphicsFamily; }
    VmaAllocator allocator() const { return m_allocator; }
    const VkPhysicalDeviceProperties& properties() const { return m_properties; }
    const DeviceFeatures& features() const { return m_features; }
    bool validationEnabled() const { return m_validation; }

    // --- Frame bookkeeping (called by the Renderer) ------------------------------
    // After the CPU waited for frame `frameNumber - kFramesInFlight`, call this
    // to run deletions that are now safe.
    void collectGarbage(uint64_t frameNumber);
    void deferDestroy(std::function<void()> fn);
    // Runs every pending deletion now (GPU must be idle).
    void flushDeletions();
    void waitIdle() const;

    // --- Resources ---------------------------------------------------------------
    Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, MemoryUsage memory,
                        const char* debugName = nullptr);
    void destroyBuffer(Buffer& buffer);

    Image createImage(const ImageDesc& desc);
    void destroyImage(Image& image);
    VkImageView createImageView(VkImage image, VkFormat format, VkImageViewType type,
                                VkImageAspectFlags aspect, uint32_t baseMip, uint32_t mipCount,
                                uint32_t baseLayer, uint32_t layerCount) const;

    // Copies CPU data into a GPU-only buffer through a temporary staging buffer.
    void uploadBuffer(const Buffer& dst, const void* data, VkDeviceSize size, VkDeviceSize dstOffset = 0);
    // Uploads pixel data into mip 0 of all layers (tightly packed, layer after
    // layer), optionally generates the remaining mips with linear blits, and
    // leaves the image in VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL.
    void uploadImage(const Image& image, const void* pixels, VkDeviceSize size, bool generateMips);

    // Records commands with `fn`, submits them and waits for completion.
    // Convenient for one-off uploads at load time; never use it per frame.
    void immediateSubmit(const std::function<void(VkCommandBuffer)>& fn);

    // Attaches a readable name to a Vulkan object (shows up in validation
    // messages and in RenderDoc). No-op when validation is off.
    void setDebugName(VkObjectType type, uint64_t handle, const char* name) const;

private:
    void createInstance(const Window& window);
    void pickPhysicalDevice();
    void createLogicalDevice();
    void createAllocator();

    bool m_validation = false;
    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties m_properties{};
    DeviceFeatures m_features{};
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    uint32_t m_graphicsFamily = UINT32_MAX;
    VmaAllocator m_allocator = nullptr;

    VkCommandPool m_uploadPool = VK_NULL_HANDLE;
    VkFence m_uploadFence = VK_NULL_HANDLE;

    struct PendingDeletion {
        uint64_t frame;
        std::function<void()> fn;
    };
    std::deque<PendingDeletion> m_deletions;
    uint64_t m_currentFrame = 0;

    PFN_vkSetDebugUtilsObjectNameEXT m_setObjectName = nullptr;
};

} // namespace ve
