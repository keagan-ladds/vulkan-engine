#pragma once

// =============================================================================
// Common Vulkan includes + the VK_CHECK error macro used everywhere in gfx/render.
// =============================================================================

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <stdexcept>
#include <string>

namespace ve::vk {
const char* resultToString(VkResult result);
bool isDepthFormat(VkFormat format);
} // namespace ve::vk

// Evaluate a VkResult-returning call and throw a descriptive error if it failed.
#define VK_CHECK(expr)                                                                      \
    do {                                                                                    \
        const VkResult veVkResult_ = (expr);                                                \
        if (veVkResult_ != VK_SUCCESS) {                                                    \
            throw std::runtime_error(std::string("Vulkan error ") +                         \
                                     ::ve::vk::resultToString(veVkResult_) + " from `" #expr \
                                     "` at " __FILE__ ":" + std::to_string(__LINE__));       \
        }                                                                                   \
    } while (0)
