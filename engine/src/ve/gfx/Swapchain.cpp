#include "ve/gfx/Swapchain.hpp"

#include "ve/gfx/Device.hpp"

#include <algorithm>

namespace ve {

void Swapchain::create(const Device& device, VkExtent2D windowExtent, bool vsync)
{
    VkPhysicalDevice gpu = device.physicalDevice();
    VkSurfaceKHR surface = device.surface();

    VkSurfaceCapabilitiesKHR caps{};
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps));

    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &count, formats.data());

    // sRGB swapchain: we output linear colour from the composite pass and the
    // hardware encodes it to sRGB for the display.
    VkSurfaceFormatKHR chosen = formats.front();
    for (const auto& f : formats) {
        if ((f.format == VK_FORMAT_B8G8R8A8_SRGB || f.format == VK_FORMAT_R8G8B8A8_SRGB) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    }

    // FIFO = vsync (always available). Without vsync prefer MAILBOX (no
    // tearing, low latency) then IMMEDIATE (may tear).
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (!vsync) {
        vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, &count, nullptr);
        std::vector<VkPresentModeKHR> modes(count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, &count, modes.data());
        if (std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != modes.end()) {
            presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
        } else if (std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_IMMEDIATE_KHR) != modes.end()) {
            presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        }
    }

    m_extent = caps.currentExtent;
    if (m_extent.width == UINT32_MAX) {
        m_extent.width = std::clamp(windowExtent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        m_extent.height = std::clamp(windowExtent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    m_minImageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0) m_minImageCount = std::min(m_minImageCount, caps.maxImageCount);

    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface;
    info.minImageCount = m_minImageCount;
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = m_extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; // single queue for everything
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = presentMode;
    info.clipped = VK_TRUE;
    VK_CHECK(vkCreateSwapchainKHR(device.handle(), &info, nullptr, &m_swapchain));
    m_format = chosen.format;

    vkGetSwapchainImagesKHR(device.handle(), m_swapchain, &count, nullptr);
    m_images.resize(count);
    vkGetSwapchainImagesKHR(device.handle(), m_swapchain, &count, m_images.data());

    m_views.resize(count);
    m_renderFinished.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        m_views[i] = device.createImageView(
            m_images[i], m_format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1);
        VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(device.handle(), &semInfo, nullptr, &m_renderFinished[i]));
    }
}

void Swapchain::destroy(const Device& device)
{
    for (VkSemaphore s : m_renderFinished) vkDestroySemaphore(device.handle(), s, nullptr);
    for (VkImageView v : m_views) vkDestroyImageView(device.handle(), v, nullptr);
    if (m_swapchain) vkDestroySwapchainKHR(device.handle(), m_swapchain, nullptr);
    m_renderFinished.clear();
    m_views.clear();
    m_images.clear();
    m_swapchain = VK_NULL_HANDLE;
}

} // namespace ve
