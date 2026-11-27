#pragma once

// =============================================================================
// Swapchain - the images presented to the window. See tinyvulkan for a
// step-by-step explanation; this version adds a vsync toggle.
// =============================================================================

#include "ve/gfx/Vk.hpp"

#include <vector>

namespace ve {

class Device;

/// The window's swapchain: the images presented on screen. Renderer internal.
class Swapchain {
public:
    /// (Re)creates it for the window's size; vsync = FIFO presentation, else mailbox / immediate.
    void create(const Device& device, VkExtent2D windowExtent, bool vsync);
    void destroy(const Device& device);  ///< Destroys it and its views.

    VkSwapchainKHR handle() const { return m_swapchain; }  ///< The Vulkan swapchain.
    VkFormat format() const { return m_format; }           ///< Image format (sRGB).
    VkExtent2D extent() const { return m_extent; }         ///< Image size in pixels.
    uint32_t imageCount() const { return static_cast<uint32_t>(m_images.size()); } ///< Number of images.
    uint32_t minImageCount() const { return m_minImageCount; } ///< The minimum it was created with.
    VkImage image(uint32_t i) const { return m_images[i]; }    ///< Image i.
    VkImageView view(uint32_t i) const { return m_views[i]; }  ///< Image i's view.
    /// One per image: the presentation engine may still hold it after the
    /// frame's fence signalled, so it can only be reused when the image is.
    VkSemaphore renderFinished(uint32_t i) const { return m_renderFinished[i]; }

private:
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkFormat m_format = VK_FORMAT_UNDEFINED;
    VkExtent2D m_extent{};
    uint32_t m_minImageCount = 2;
    std::vector<VkImage> m_images;
    std::vector<VkImageView> m_views;
    std::vector<VkSemaphore> m_renderFinished;
};

} // namespace ve
