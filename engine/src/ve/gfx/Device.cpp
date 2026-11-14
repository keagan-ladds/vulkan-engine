#include "ve/gfx/Device.hpp"

#include "ve/core/Log.hpp"
#include "ve/core/Window.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace ve {

namespace {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
{
    // Every mesh pipeline uses the fixed ve::Vertex layout, so shaders that
    // ignore some attributes (e.g. shadow or water shaders) are expected.
    // That's by design; don't spam the log about it.
    if (data->pMessageIdName && std::strstr(data->pMessageIdName, "Shader-OutputNotConsumed")) return VK_FALSE;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        log::error("[vulkan] {}", data->pMessage);
    } else {
        log::warn("[vulkan] {}", data->pMessage);
    }
    return VK_FALSE;
}

VkDebugUtilsMessengerCreateInfoEXT debugMessengerInfo()
{
    VkDebugUtilsMessengerCreateInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = debugCallback;
    return info;
}

bool hasLayer(const char* name)
{
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    return std::any_of(layers.begin(), layers.end(),
                       [&](const VkLayerProperties& l) { return std::strcmp(l.layerName, name) == 0; });
}

bool hasDeviceExtension(VkPhysicalDevice gpu, const char* name)
{
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, exts.data());
    return std::any_of(exts.begin(), exts.end(),
                       [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
}

// Feature structs chained together, used both to query and to enable.
struct FeatureChain {
    VkPhysicalDeviceFeatures2 core{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};

    FeatureChain()
    {
        core.pNext = &v12;
        v12.pNext = &v13;
    }
    FeatureChain(const FeatureChain&) = delete;
};

// Features the engine cannot run without.
bool supportsRequired(const FeatureChain& f)
{
    return f.v13.dynamicRendering && f.v13.synchronization2 &&
           f.v12.descriptorIndexing && f.v12.runtimeDescriptorArray &&
           f.v12.descriptorBindingPartiallyBound &&
           f.v12.descriptorBindingSampledImageUpdateAfterBind &&
           f.v12.descriptorBindingStorageImageUpdateAfterBind &&
           f.v12.descriptorBindingUpdateUnusedWhilePending &&
           f.v12.shaderSampledImageArrayNonUniformIndexing &&
           f.core.features.imageCubeArray && // point shadow cube arrays
           f.core.features.drawIndirectFirstInstance; // GPU-driven instancing
}

} // namespace

const char* vk::resultToString(VkResult r)
{
    switch (r) {
#define VE_CASE(x) case x: return #x
        VE_CASE(VK_SUCCESS); VE_CASE(VK_NOT_READY); VE_CASE(VK_TIMEOUT); VE_CASE(VK_INCOMPLETE);
        VE_CASE(VK_SUBOPTIMAL_KHR); VE_CASE(VK_ERROR_OUT_OF_HOST_MEMORY);
        VE_CASE(VK_ERROR_OUT_OF_DEVICE_MEMORY); VE_CASE(VK_ERROR_INITIALIZATION_FAILED);
        VE_CASE(VK_ERROR_DEVICE_LOST); VE_CASE(VK_ERROR_MEMORY_MAP_FAILED);
        VE_CASE(VK_ERROR_LAYER_NOT_PRESENT); VE_CASE(VK_ERROR_EXTENSION_NOT_PRESENT);
        VE_CASE(VK_ERROR_FEATURE_NOT_PRESENT); VE_CASE(VK_ERROR_INCOMPATIBLE_DRIVER);
        VE_CASE(VK_ERROR_TOO_MANY_OBJECTS); VE_CASE(VK_ERROR_FORMAT_NOT_SUPPORTED);
        VE_CASE(VK_ERROR_FRAGMENTED_POOL); VE_CASE(VK_ERROR_OUT_OF_POOL_MEMORY);
        VE_CASE(VK_ERROR_SURFACE_LOST_KHR); VE_CASE(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR);
        VE_CASE(VK_ERROR_OUT_OF_DATE_KHR);
#undef VE_CASE
    default: return "VK_RESULT_UNKNOWN";
    }
}

bool vk::isDepthFormat(VkFormat f)
{
    return f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D24_UNORM_S8_UINT ||
           f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_X8_D24_UNORM_PACK32;
}

// -----------------------------------------------------------------------------
// Init / shutdown
// -----------------------------------------------------------------------------

void Device::init(const Window& window, bool enableValidation)
{
    m_validation = enableValidation;
    if (m_validation && !hasLayer(kValidationLayer)) {
        log::warn("Validation requested but {} is not installed; continuing without it", kValidationLayer);
        m_validation = false;
    }

    createInstance(window);
    m_surface = window.createSurface(m_instance);
    pickPhysicalDevice();
    createLogicalDevice();
    createAllocator();

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.queueFamilyIndex = m_graphicsFamily;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    VK_CHECK(vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_uploadPool));
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_CHECK(vkCreateFence(m_device, &fenceInfo, nullptr, &m_uploadFence));
}

void Device::shutdown()
{
    if (!m_device) return;
    vkDeviceWaitIdle(m_device);

    flushDeletions();

    vkDestroyFence(m_device, m_uploadFence, nullptr);
    vkDestroyCommandPool(m_device, m_uploadPool, nullptr);
    vmaDestroyAllocator(m_allocator);
    vkDestroyDevice(m_device, nullptr);
    vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    if (m_debugMessenger) {
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy) destroy(m_instance, m_debugMessenger, nullptr);
    }
    vkDestroyInstance(m_instance, nullptr);
    m_device = VK_NULL_HANDLE;
}

void Device::createInstance(const Window& window)
{
    uint32_t loaderVersion = 0;
    vkEnumerateInstanceVersion(&loaderVersion);
    if (loaderVersion < VK_API_VERSION_1_3) {
        throw std::runtime_error("A Vulkan 1.3 loader/driver is required");
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "vulkan-engine";
    app.pEngineName = "ve";
    app.apiVersion = VK_API_VERSION_1_3;

    std::vector<const char*> extensions = window.requiredVulkanExtensions();
    if (m_validation) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();

    VkDebugUtilsMessengerCreateInfoEXT dbg = debugMessengerInfo();
    if (m_validation) {
        info.enabledLayerCount = 1;
        info.ppEnabledLayerNames = &kValidationLayer;
        info.pNext = &dbg; // also report problems during vkCreate/DestroyInstance
    }
    VK_CHECK(vkCreateInstance(&info, nullptr, &m_instance));

    if (m_validation) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
        VK_CHECK(create(m_instance, &dbg, nullptr, &m_debugMessenger));
        m_setObjectName = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
            vkGetInstanceProcAddr(m_instance, "vkSetDebugUtilsObjectNameEXT"));
    }
}

void Device::pickPhysicalDevice()
{
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    std::vector<VkPhysicalDevice> gpus(count);
    vkEnumeratePhysicalDevices(m_instance, &count, gpus.data());

    int bestScore = -1;
    for (VkPhysicalDevice gpu : gpus) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(gpu, &props);
        if (props.apiVersion < VK_API_VERSION_1_3) continue;
        if (!hasDeviceExtension(gpu, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;

        FeatureChain f;
        vkGetPhysicalDeviceFeatures2(gpu, &f.core);
        if (!supportsRequired(f)) continue;

        // We use a single queue for graphics, compute, transfer and present.
        // Every desktop GPU has such a queue family.
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &familyCount, families.data());
        uint32_t family = UINT32_MAX;
        for (uint32_t i = 0; i < familyCount; ++i) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, m_surface, &present);
            const VkQueueFlags need = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
            if ((families[i].queueFlags & need) == need && present) {
                family = i;
                break;
            }
        }
        if (family == UINT32_MAX) continue;

        int score = 1;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score = 1000;
        else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score = 100;
        if (score > bestScore) {
            bestScore = score;
            m_physicalDevice = gpu;
            m_graphicsFamily = family;
            m_properties = props;
        }
    }
    if (!m_physicalDevice) {
        throw std::runtime_error("No GPU supports Vulkan 1.3 with descriptor indexing + cube arrays");
    }
    log::info("GPU: {} (Vulkan {}.{})", m_properties.deviceName,
              VK_API_VERSION_MAJOR(m_properties.apiVersion), VK_API_VERSION_MINOR(m_properties.apiVersion));
}

void Device::createLogicalDevice()
{
    FeatureChain supported;
    vkGetPhysicalDeviceFeatures2(m_physicalDevice, &supported.core);

    // Optional features: use them if present.
    m_features.samplerAnisotropy = supported.core.features.samplerAnisotropy;
    m_features.maxAnisotropy = m_features.samplerAnisotropy ? m_properties.limits.maxSamplerAnisotropy : 1.0f;
    m_features.depthClamp = supported.core.features.depthClamp;
    m_features.fillModeNonSolid = supported.core.features.fillModeNonSolid;

    FeatureChain enable;
    enable.core.features.samplerAnisotropy = m_features.samplerAnisotropy;
    enable.core.features.depthClamp = m_features.depthClamp;
    enable.core.features.fillModeNonSolid = m_features.fillModeNonSolid;
    enable.core.features.imageCubeArray = VK_TRUE;
    enable.core.features.drawIndirectFirstInstance = VK_TRUE;
    enable.core.features.shaderStorageImageWriteWithoutFormat =
        supported.core.features.shaderStorageImageWriteWithoutFormat;
    // Descriptor indexing = "bindless" textures (see gfx/Bindless.hpp).
    enable.v12.descriptorIndexing = VK_TRUE;
    enable.v12.runtimeDescriptorArray = VK_TRUE;
    enable.v12.descriptorBindingPartiallyBound = VK_TRUE;
    enable.v12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    enable.v12.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
    enable.v12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    enable.v12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    enable.v12.shaderStorageImageArrayNonUniformIndexing = supported.v12.shaderStorageImageArrayNonUniformIndexing;
    enable.v12.scalarBlockLayout = supported.v12.scalarBlockLayout;
    enable.v12.hostQueryReset = supported.v12.hostQueryReset;
    enable.v13.dynamicRendering = VK_TRUE;
    enable.v13.synchronization2 = VK_TRUE;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = m_graphicsFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.pNext = &enable.core;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queueInfo;
    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = extensions;
    VK_CHECK(vkCreateDevice(m_physicalDevice, &info, nullptr, &m_device));
    vkGetDeviceQueue(m_device, m_graphicsFamily, 0, &m_graphicsQueue);
}

void Device::createAllocator()
{
    VmaAllocatorCreateInfo info{};
    info.physicalDevice = m_physicalDevice;
    info.device = m_device;
    info.instance = m_instance;
    info.vulkanApiVersion = VK_API_VERSION_1_3;
    VK_CHECK(vmaCreateAllocator(&info, &m_allocator));
}

// -----------------------------------------------------------------------------
// Deferred deletion
// -----------------------------------------------------------------------------

void Device::deferDestroy(std::function<void()> fn)
{
    m_deletions.push_back({m_currentFrame, std::move(fn)});
}

void Device::collectGarbage(uint64_t frameNumber)
{
    m_currentFrame = frameNumber;
    // Something queued during frame F was possibly used by frame F's GPU work.
    // Once we're starting frame F + kFramesInFlight, the renderer has waited on
    // the fence of frame F, so it's safe to destroy.
    while (!m_deletions.empty() && m_deletions.front().frame + kFramesInFlight <= frameNumber) {
        m_deletions.front().fn();
        m_deletions.pop_front();
    }
}

void Device::flushDeletions()
{
    // A deletion may queue further deletions, so loop until empty.
    while (!m_deletions.empty()) {
        auto fn = std::move(m_deletions.front().fn);
        m_deletions.pop_front();
        fn();
    }
}

void Device::waitIdle() const
{
    vkDeviceWaitIdle(m_device);
}

// -----------------------------------------------------------------------------
// Buffers
// -----------------------------------------------------------------------------

Buffer Device::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, MemoryUsage memory,
                            const char* debugName)
{
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    if (memory == MemoryUsage::GpuOnly) info.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    if (memory == MemoryUsage::CpuToGpu) {
        alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    } else if (memory == MemoryUsage::GpuToCpu) {
        alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }
    // Coherent memory: CPU writes are visible to the GPU without explicit
    // flushes (we memcpy into mapped buffers every frame).
    if (memory != MemoryUsage::GpuOnly) alloc.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    Buffer buffer;
    buffer.size = size;
    VmaAllocationInfo allocInfo{};
    VK_CHECK(vmaCreateBuffer(m_allocator, &info, &alloc, &buffer.handle, &buffer.allocation, &allocInfo));
    buffer.mapped = allocInfo.pMappedData;
    if (debugName) setDebugName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(buffer.handle), debugName);
    return buffer;
}

void Device::destroyBuffer(Buffer& buffer)
{
    if (buffer.handle) vmaDestroyBuffer(m_allocator, buffer.handle, buffer.allocation);
    buffer = Buffer{};
}

void Device::uploadBuffer(const Buffer& dst, const void* data, VkDeviceSize size, VkDeviceSize dstOffset)
{
    Buffer staging = createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUsage::CpuToGpu);
    std::memcpy(staging.mapped, data, static_cast<size_t>(size));
    immediateSubmit([&](VkCommandBuffer cmd) {
        VkBufferCopy region{0, dstOffset, size};
        vkCmdCopyBuffer(cmd, staging.handle, dst.handle, 1, &region);
    });
    destroyBuffer(staging);
}

// -----------------------------------------------------------------------------
// Images
// -----------------------------------------------------------------------------

VkImageView Device::createImageView(VkImage image, VkFormat format, VkImageViewType type,
                                    VkImageAspectFlags aspect, uint32_t baseMip, uint32_t mipCount,
                                    uint32_t baseLayer, uint32_t layerCount) const
{
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = image;
    info.viewType = type;
    info.format = format;
    info.subresourceRange = {aspect, baseMip, mipCount, baseLayer, layerCount};
    VkImageView view = VK_NULL_HANDLE;
    VK_CHECK(vkCreateImageView(m_device, &info, nullptr, &view));
    return view;
}

Image Device::createImage(const ImageDesc& desc)
{
    Image img;
    img.format = desc.format;
    img.extent = desc.extent;
    img.mipLevels = desc.mipLevels;
    img.layers = desc.layers;
    img.viewType = desc.viewType;
    img.aspect = vk::isDepthFormat(desc.format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;

    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = desc.viewType == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    info.format = desc.format;
    info.extent = desc.extent;
    info.mipLevels = desc.mipLevels;
    info.arrayLayers = desc.layers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = desc.usage;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (desc.viewType == VK_IMAGE_VIEW_TYPE_CUBE || desc.viewType == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) {
        info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }

    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO; // VMA picks device-local memory for optimal-tiling images
    VK_CHECK(vmaCreateImage(m_allocator, &info, &alloc, &img.handle, &img.allocation, nullptr));

    img.view = createImageView(img.handle, desc.format, desc.viewType, img.aspect, 0, desc.mipLevels, 0, desc.layers);
    if (desc.perLayerViews) {
        img.layerViews.resize(desc.layers);
        for (uint32_t i = 0; i < desc.layers; ++i) {
            img.layerViews[i] = createImageView(img.handle, desc.format, VK_IMAGE_VIEW_TYPE_2D, img.aspect, 0, 1, i, 1);
        }
    }
    if (desc.debugName) {
        setDebugName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(img.handle), desc.debugName);
    }
    return img;
}

void Device::destroyImage(Image& image)
{
    for (VkImageView v : image.layerViews) vkDestroyImageView(m_device, v, nullptr);
    if (image.view) vkDestroyImageView(m_device, image.view, nullptr);
    if (image.handle) vmaDestroyImage(m_allocator, image.handle, image.allocation);
    image = Image{};
}

void Device::uploadImage(const Image& image, const void* pixels, VkDeviceSize size, bool generateMips)
{
    Buffer staging = createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUsage::CpuToGpu);
    std::memcpy(staging.mapped, pixels, static_cast<size_t>(size));

    immediateSubmit([&](VkCommandBuffer cmd) {
        auto barrier = [&](uint32_t baseMip, uint32_t mipCount, VkImageLayout from, VkImageLayout to,
                           VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                           VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
            VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.srcStageMask = srcStage;
            b.srcAccessMask = srcAccess;
            b.dstStageMask = dstStage;
            b.dstAccessMask = dstAccess;
            b.oldLayout = from;
            b.newLayout = to;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = image.handle;
            b.subresourceRange = {image.aspect, baseMip, mipCount, 0, image.layers};
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2(cmd, &dep);
        };

        // Whole image -> TRANSFER_DST, copy mip 0.
        barrier(0, image.mipLevels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

        VkBufferImageCopy copy{};
        copy.imageSubresource = {image.aspect, 0, 0, image.layers};
        copy.imageExtent = image.extent;
        vkCmdCopyBufferToImage(cmd, staging.handle, image.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        // Build the mip chain: each level is a half-size linear blit of the previous.
        int32_t w = static_cast<int32_t>(image.extent.width);
        int32_t h = static_cast<int32_t>(image.extent.height);
        int32_t d = static_cast<int32_t>(image.extent.depth);
        const uint32_t levels = generateMips ? image.mipLevels : 1;
        for (uint32_t mip = 1; mip < levels; ++mip) {
            barrier(mip - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            const int32_t nw = std::max(1, w / 2), nh = std::max(1, h / 2), nd = std::max(1, d / 2);
            VkImageBlit blit{};
            blit.srcSubresource = {image.aspect, mip - 1, 0, image.layers};
            blit.srcOffsets[1] = {w, h, d};
            blit.dstSubresource = {image.aspect, mip, 0, image.layers};
            blit.dstOffsets[1] = {nw, nh, nd};
            vkCmdBlitImage(cmd, image.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image.handle,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
            barrier(mip - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            w = nw; h = nh; d = nd;
        }
        // Remaining levels (the last generated mip, or all of them if no
        // mipmaps were generated) are still TRANSFER_DST.
        const uint32_t firstRemaining = levels - 1;
        barrier(firstRemaining, image.mipLevels - firstRemaining,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });
    destroyBuffer(staging);
}

void Device::immediateSubmit(const std::function<void(VkCommandBuffer)>& fn)
{
    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool = m_uploadPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(m_device, &allocInfo, &cmd));

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &begin));
    fn(cmd);
    VK_CHECK(vkEndCommandBuffer(cmd));

    VkCommandBufferSubmitInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmdInfo.commandBuffer = cmd;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;
    VK_CHECK(vkQueueSubmit2(m_graphicsQueue, 1, &submit, m_uploadFence));
    VK_CHECK(vkWaitForFences(m_device, 1, &m_uploadFence, VK_TRUE, UINT64_MAX));
    VK_CHECK(vkResetFences(m_device, 1, &m_uploadFence));
    vkFreeCommandBuffers(m_device, m_uploadPool, 1, &cmd);
}

void Device::setDebugName(VkObjectType type, uint64_t handle, const char* name) const
{
    if (!m_setObjectName || !name) return;
    VkDebugUtilsObjectNameInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    info.objectType = type;
    info.objectHandle = handle;
    info.pObjectName = name;
    m_setObjectName(m_device, &info);
}

} // namespace ve
