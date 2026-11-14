#include "ve/core/Window.hpp"

#include <SDL.h>
#include <SDL_vulkan.h>

#include <stdexcept>

namespace ve {

Window::Window(const std::string& title, int width, int height)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) {
        throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
    }

    const Uint32 flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    m_window = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                width, height, flags);
    if (!m_window) {
        const std::string error = SDL_GetError();
        SDL_Quit();
        throw std::runtime_error("SDL_CreateWindow failed: " + error);
    }
}

Window::~Window()
{
    if (m_window) {
        SDL_DestroyWindow(m_window);
    }
    SDL_Quit();
}

void Window::processEvent(const SDL_Event& event)
{
    if (event.type == SDL_QUIT) {
        m_shouldClose = true;
    } else if (event.type == SDL_WINDOWEVENT) {
        switch (event.window.event) {
        case SDL_WINDOWEVENT_SIZE_CHANGED: m_resized = true; break;
        case SDL_WINDOWEVENT_MINIMIZED: m_minimized = true; break;
        case SDL_WINDOWEVENT_RESTORED:
        case SDL_WINDOWEVENT_MAXIMIZED:
            m_minimized = false;
            m_resized = true;
            break;
        default: break;
        }
    }
}

bool Window::consumeResized()
{
    const bool r = m_resized;
    m_resized = false;
    return r;
}

void Window::waitForEvent() const
{
    SDL_WaitEventTimeout(nullptr, 100);
}

void Window::setTitle(const std::string& title)
{
    SDL_SetWindowTitle(m_window, title.c_str());
}

VkExtent2D Window::drawableSize() const
{
    int w = 0, h = 0;
    SDL_Vulkan_GetDrawableSize(m_window, &w, &h);
    return {static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
}

std::vector<const char*> Window::requiredVulkanExtensions() const
{
    unsigned int count = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(m_window, &count, nullptr)) {
        throw std::runtime_error(std::string("SDL_Vulkan_GetInstanceExtensions: ") + SDL_GetError());
    }
    std::vector<const char*> exts(count);
    SDL_Vulkan_GetInstanceExtensions(m_window, &count, exts.data());
    return exts;
}

VkSurfaceKHR Window::createSurface(VkInstance instance) const
{
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(m_window, instance, &surface)) {
        throw std::runtime_error(std::string("SDL_Vulkan_CreateSurface: ") + SDL_GetError());
    }
    return surface;
}

std::filesystem::path executableDirectory()
{
    char* base = SDL_GetBasePath();
    if (!base) {
        return std::filesystem::current_path();
    }
    std::filesystem::path p(base);
    SDL_free(base);
    return p;
}

} // namespace ve
