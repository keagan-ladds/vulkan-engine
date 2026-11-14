#pragma once

// =============================================================================
// Engine - creates every subsystem and runs the main loop.
//
//   int main() {
//       ve::EngineConfig config;
//       config.title = "My Game";
//       ve::Engine engine(config);
//       engine.scene().spawn<Player>();
//       engine.run();
//   }
//
// Anywhere in game code: ve::Engine::get().input() etc., or the shortcuts on
// GameObject/Component (input(), time(), renderer(), assets(), audio()).
// See docs/01-architecture.md §3 for the exact order of a frame.
// =============================================================================

#include "ve/core/Input.hpp"
#include "ve/core/Time.hpp"
#include "ve/render/Renderer.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ve {

class Window;
class Scene;
class AssetManager;
class AudioSystem;
class UiLayer;

struct EngineConfig {
    std::string title = "vulkan-engine";
    int width = 1600;
    int height = 900;
#ifdef NDEBUG
    bool validation = false;
#else
    bool validation = true; // Vulkan validation layers in Debug builds
#endif
    RendererSettings renderer;
    float fixedTimestep = 1.0f / 50.0f;
    int maxFixedStepsPerFrame = 5; // avoids the "spiral of death" after a hitch
    bool enableAudio = true;
    // Extra asset / shader folders (relative to the executable or absolute).
    // <exe>/assets and <exe>/shaders are always searched.
    std::vector<std::filesystem::path> assetPaths;
    std::vector<std::filesystem::path> shaderPaths;
};

class Engine {
public:
    explicit Engine(const EngineConfig& config = {});
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    static Engine& get();

    void run();  // returns when quit() is called or the window closes
    void quit();

    Window& window() { return *m_window; }
    Input& input() { return m_input; }
    Time& time() { return m_time; }
    Renderer& renderer() { return *m_renderer; }
    Scene& scene() { return *m_scene; }
    AssetManager& assets() { return *m_assets; }
    AudioSystem& audio() { return *m_audio; }
    UiLayer& ui() { return *m_ui; }
    const EngineConfig& config() const { return m_config; }

private:
    void pollEvents();
    void tickFrame();

    EngineConfig m_config;
    Input m_input;
    Time m_time;
    std::unique_ptr<Window> m_window;
    std::unique_ptr<Renderer> m_renderer;
    std::unique_ptr<AudioSystem> m_audio;
    std::unique_ptr<AssetManager> m_assets;
    std::unique_ptr<UiLayer> m_ui;
    std::unique_ptr<Scene> m_scene;
    bool m_running = false;
    double m_accumulator = 0.0;
};

} // namespace ve
