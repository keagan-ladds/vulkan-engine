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

/// The OS window (SDL): events, size, title, the Vulkan surface. Owned by the Engine.
class Window {
public:
    /// Opens the window (and initialises SDL).
    /// @param width, height initial size in screen pixels.
    /// @param hidden create it hidden (headless runs; the Engine shows it with the first frame).
    Window(const std::string& title, int width, int height, bool hidden = false);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    /// Engine-internal: handles window events (close, resize, minimise).
    void processEvent(const SDL_Event& event);

    /// The user asked to close the window (or requestClose() was called).
    bool shouldClose() const { return m_shouldClose; }
    /// Makes shouldClose() true: the Engine's loop ends after this frame.
    void requestClose() { m_shouldClose = true; }

    /// Whether the window was resized since the last call (and clears the flag).
    bool consumeResized();
    /// Whether the window is minimised (the Engine stops rendering meanwhile).
    bool isMinimized() const { return m_minimized; }
    void waitForEvent() const; ///< sleep until something happens (used while minimized)

    /// Changes the window title.
    void setTitle(const std::string& title);
    void show(); ///< (created hidden: shown once there is something to see)

    VkExtent2D drawableSize() const; ///< pixels (HiDPI aware)
    /// Instance extensions Vulkan needs to present to this window.
    std::vector<const char*> requiredVulkanExtensions() const;
    /// Creates the Vulkan surface for this window (the caller owns it).
    VkSurfaceKHR createSurface(VkInstance instance) const;

    /// The underlying SDL window.
    SDL_Window* handle() const { return m_window; }

private:
    SDL_Window* m_window = nullptr;
    bool m_shouldClose = false;
    bool m_resized = false;
    bool m_minimized = false;
};

/// Folder that contains the running executable (assets/shaders live next to it).
std::filesystem::path executableDirectory();

} // namespace ve
