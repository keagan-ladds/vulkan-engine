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
#include <span>
#include <vector>

namespace ve {

class Window;

/// Optional GPU features the device was created with (Device::features()).
struct DeviceFeatures {
    bool samplerAnisotropy = false;  ///< Anisotropic filtering is available.
    float maxAnisotropy = 1.0f;  ///< Highest anisotropy level supported.
    bool depthClamp = false;  ///< Depth clamping (shadow casters behind the near plane).
    bool fillModeNonSolid = false;  ///< Wireframe rendering.
    /// BC1-7 block-compressed textures (every desktop GPU): cooked textures use
    /// BC4 / BC5 / BC7. Without it the asset manager loads the source images.
    bool textureCompressionBC = false;
};

/// Fills mips 1..n of `image` by successive half-size blits of mip 0 (which is
/// in `mip0Layout`, last written at mip0Stage/mip0Access); leaves every level
/// READ_ONLY_OPTIMAL.
void recordMipChain(VkCommandBuffer cmd, const Image& image, VkImageLayout mip0Layout, VkPipelineStageFlags2 mip0Stage,
                    VkAccessFlags2 mip0Access);

/// The Vulkan instance, physical / logical device, queue and memory
/// allocator (VMA), plus deferred destruction and uploads. Owned by the Renderer.
class Device {
public:
    /// Frames the CPU may record ahead of the GPU (per-frame resources are doubled).
    static constexpr uint32_t kFramesInFlight = 2;

    Device() = default;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    /// Creates the instance (with validation layers if asked), picks a GPU, creates the device.
    void init(const Window& window, bool enableValidation);
    /// Destroys everything (the GPU must be idle).
    void shutdown();

    // --- Handles ---------------------------------------------------------------
    /// The Vulkan instance.
    VkInstance instance() const { return m_instance; }
    /// The GPU in use.
    VkPhysicalDevice physicalDevice() const { return m_physicalDevice; }
    /// The logical device.
    VkDevice handle() const { return m_device; }
    /// The window's surface.
    VkSurfaceKHR surface() const { return m_surface; }
    /// The graphics (and present) queue.
    VkQueue graphicsQueue() const { return m_graphicsQueue; }
    /// Its queue family index.
    uint32_t graphicsFamily() const { return m_graphicsFamily; }
    /// The VMA allocator.
    VmaAllocator allocator() const { return m_allocator; }
    /// The GPU's properties and limits.
    const VkPhysicalDeviceProperties& properties() const { return m_properties; }
    /// The optional features enabled.
    const DeviceFeatures& features() const { return m_features; }
    /// Whether validation layers are on.
    bool validationEnabled() const { return m_validation; }
    /// Shared by every pipeline (see setPipelineCache in Pipeline.hpp); loaded
    /// from / saved to "pipeline_cache.bin" next to the executable.
    VkPipelineCache pipelineCache() const { return m_pipelineCache; }

    // --- Frame bookkeeping (called by the Renderer) ------------------------------
    /// After the CPU waited for frame `frameNumber - kFramesInFlight`, call this
    /// to run deletions that are now safe.
    void collectGarbage(uint64_t frameNumber);
    /// Runs `fn` once the GPU can no longer use what it destroys (kFramesInFlight frames later).
    void deferDestroy(std::function<void()> fn);
    /// Runs every pending deletion now (GPU must be idle).
    void flushDeletions();
    /// Waits until the GPU is idle (slow: shutdown, resizes).
    void waitIdle() const;

    // --- Resources ---------------------------------------------------------------
    /// Creates a buffer.
    /// @param memory where it lives (GPU only, CPU-visible for uploads / readback...).
    /// @param debugName shown in validation messages and GPU debuggers.
    Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, MemoryUsage memory,
                        const char* debugName = nullptr);
    /// Destroys a buffer now (only when the GPU no longer uses it; else deferDestroy).
    void destroyBuffer(Buffer& buffer);

    /// Creates an image (and its default view) from `desc`.
    Image createImage(const ImageDesc& desc);
    /// Destroys an image now (only when the GPU no longer uses it; else deferDestroy).
    void destroyImage(Image& image);
    /// Creates a view of part of an image (mips, layers, swizzle); the caller destroys it.
    VkImageView createImageView(VkImage image, VkFormat format, VkImageViewType type,
                                VkImageAspectFlags aspect, uint32_t baseMip, uint32_t mipCount,
                                uint32_t baseLayer, uint32_t layerCount, VkComponentMapping swizzle = {}) const;

    /// Asynchronous buffer uploads. The data is copied into staging memory right
    /// away (the caller may free it on return); the GPU copy is recorded at the
    /// start of the next frame's command buffer (flushUploads), or into the next
    /// immediateSubmit if that comes first, so it lands before any later use on
    /// the queue without stalling the CPU. The destination must stay alive until
    /// then: destroyBuffer() drops copies into a buffer destroyed before they
    /// were recorded, and a buffer released with deferDestroy() is kept alive
    /// until the frame that flushed its copies has finished.
    struct BufferUpload {
        const Buffer* dst = nullptr;  ///< The buffer to write.
        const void* data = nullptr;  ///< The bytes (copied right away).
        VkDeviceSize size = 0;  ///< How many bytes.
        VkDeviceSize dstOffset = 0;  ///< Where in the buffer.
    };
    /// Queues one upload (see above).
    void uploadBuffer(const Buffer& dst, const void* data, VkDeviceSize size, VkDeviceSize dstOffset = 0);
    /// Queues several uploads (see above).
    void uploadBuffers(std::span<const BufferUpload> uploads);
    /// Same, but blocks until the data is on the GPU (one immediateSubmit), for
    /// code that reads it outside a frame without an immediateSubmit of its own.
    void uploadBuffersNow(std::span<const BufferUpload> uploads);
    /// Renderer: records every queued buffer copy into the frame's command
    /// buffer, between barriers against earlier GPU use of the destinations and
    /// later reads of the new data. Call right after vkBeginCommandBuffer.
    void flushUploads(VkCommandBuffer cmd);
    /// Uploads pixel data into mip 0 of all layers (tightly packed, layer after
    /// layer), optionally generates the remaining mips with linear blits, and
    /// leaves the image in VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL.
    void uploadImage(const Image& image, const void* pixels, VkDeviceSize size, bool generateMips);
    /// Uploads every mip level of a 2D image from one buffer (levelOffsets[i] =
    /// where level i starts; any format, block-compressed included), then
    /// leaves it READ_ONLY_OPTIMAL. For prebuilt mip chains (cooked textures).
    void uploadImageLevels(const Image& image, const void* data, VkDeviceSize size, std::span<const VkDeviceSize> levelOffsets);

    /// Records commands with `fn`, submits them and waits for completion.
    /// Convenient for one-off uploads at load time; never use it per frame.
    /// Queued buffer uploads are recorded first, so `fn` may use their data.
    void immediateSubmit(const std::function<void(VkCommandBuffer)>& fn);

    /// Attaches a readable name to a Vulkan object (shows up in validation
    /// messages and in RenderDoc). No-op when validation is off.
    void setDebugName(VkObjectType type, uint64_t handle, const char* name) const;

private:
    void createInstance(const Window& window);
    void pickPhysicalDevice();
    void createLogicalDevice();
    void createAllocator();
    void createPipelineCache();
    void savePipelineCache();

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
    VkPipelineCache m_pipelineCache = VK_NULL_HANDLE;

    // --- Asynchronous uploads (see uploadBuffers) ---------------------------------
    /// Staging memory comes in blocks: the open block takes the uploads until the
    /// next flush; the flush retires it, tagged with the frame whose command
    /// buffer copies from it, and it's reused once that frame's fence has passed
    /// (collectGarbage, same rule as deferred deletion): in effect one block per
    /// frame in flight, grown on demand.
    struct PendingCopy {
        VkBuffer src; ///< VK_NULL_HANDLE: the open staging block (it may still be regrown)
        VkDeviceSize srcOffset;
        VkBuffer dst;
        VkDeviceSize dstOffset;
        VkDeviceSize size;
    };
    struct RetiredStaging {
        uint64_t frame;
        Buffer buffer;
    };
    VkDeviceSize allocateStaging(VkDeviceSize size); ///< offset in the open block
    /// Records the queued copies between their barriers; false if there were none.
    bool recordPendingCopies(VkCommandBuffer cmd);
    void recycleStaging(Buffer buffer);

    Buffer m_staging; ///< the open block
    VkDeviceSize m_stagingUsed = 0;
    std::vector<Buffer> m_stagingFree;
    std::deque<RetiredStaging> m_stagingRetired;
    std::vector<Buffer> m_dedicatedStaging; ///< very large uploads, released after their flush
    std::vector<PendingCopy> m_pendingCopies;

    struct PendingDeletion {
        uint64_t frame;
        std::function<void()> fn;
    };
    std::deque<PendingDeletion> m_deletions;
    uint64_t m_currentFrame = 0;

    PFN_vkSetDebugUtilsObjectNameEXT m_setObjectName = nullptr;
};

} // namespace ve
