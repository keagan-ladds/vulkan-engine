#pragma once

// =============================================================================
// Window - SDL2 window wrapper (the platform layer).
//
// The Engine polls SDL events and hands each one to processEvent(); the window
// keeps track of resize/minimize/close. The renderer only needs the Vulkan
// helpers at the bottom.
// =============================================================================

#include <vulkan/vulkan.h>

#include <filesystem>
#include <string>
#include <vector>

struct SDL_Window;
union SDL_Event;

namespace ve {

class Window {
public:
    Window(const std::string& title, int width, int height, bool hidden = false);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    void processEvent(const SDL_Event& event);

    bool shouldClose() const { return m_shouldClose; }
    void requestClose() { m_shouldClose = true; }

    bool consumeResized();
    bool isMinimized() const { return m_minimized; }
    void waitForEvent() const; // sleep until something happens (used while minimized)

    void setTitle(const std::string& title);

    VkExtent2D drawableSize() const; // pixels (HiDPI aware)
    std::vector<const char*> requiredVulkanExtensions() const;
    VkSurfaceKHR createSurface(VkInstance instance) const;

    SDL_Window* handle() const { return m_window; }

private:
    SDL_Window* m_window = nullptr;
    bool m_shouldClose = false;
    bool m_resized = false;
    bool m_minimized = false;
};

// Folder that contains the running executable (assets/shaders live next to it).
std::filesystem::path executableDirectory();

} // namespace ve
