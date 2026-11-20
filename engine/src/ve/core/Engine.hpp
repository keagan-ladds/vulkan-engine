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
//
// This header is the engine-wide service locator, so every module may include
// it: it only forward-declares the subsystems (include e.g. ve/render/
// Renderer.hpp to use renderer()). The implementation, which creates and
// wires every module, is the composition root in ve/app/Engine.cpp.
// =============================================================================

#include "ve/core/Jobs.hpp" // the worker pool the Engine shuts down (also sizes physics threads)
#include "ve/core/Input.hpp"
#include "ve/core/Time.hpp"
#include "ve/render/RendererSettings.hpp" // plain data held by EngineConfig

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ve {

class Window;
class Renderer;
class Scene;
class AssetManager;
class AudioSystem;
class UiLayer;
class Editor;
class PhysicsWorld;
namespace net {
class Network;
}

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
    bool enableEditor = true; // F2 opens the in-engine scene editor (docs/05-editor.md)
    // Extra asset / shader folders (relative to the executable or absolute).
    // <exe>/assets and <exe>/shaders are always searched.
    std::vector<std::filesystem::path> assetPaths;
    std::vector<std::filesystem::path> shaderPaths;
    // Save games (ve::save): empty = the platform's per-user data folder ("<title>/saves").
    std::filesystem::path saveFolder;
};

class Engine {
public:
    explicit Engine(const EngineConfig& config = {});
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    static Engine& get();
    static bool exists();

    void run();  // returns when quit() is called or the window closes
    void quit();
    // Runs at shutdown after the scene is gone, before the renderer: release
    // GPU resources the game keeps outside the scene (e.g. baked icons in a
    // static table).
    void atShutdown(std::function<void()> fn) { m_atShutdown.push_back(std::move(fn)); }

    Window& window() { return *m_window; }
    Input& input() { return m_input; }
    Time& time() { return m_time; }
    Renderer& renderer() { return *m_renderer; }
    Scene& scene() { return *m_scene; }
    AssetManager& assets() { return *m_assets; }
    AudioSystem& audio() { return *m_audio; }
    net::Network& net() { return *m_net; } // multiplayer (offline until host / join)
    PhysicsWorld& physics() { return *m_physics; }
    UiLayer& ui() { return *m_ui; }
    Editor* editor() { return m_editor.get(); } // null if EngineConfig::enableEditor is false
    // The editor is in Edit mode (game paused); false without an editor. For
    // modules that may not include the editor (ui, gameplay, ...).
    bool editorPaused() const;
    const EngineConfig& config() const { return m_config; }

    // Pauses gameplay like the editor's Edit mode: no fixed steps or physics,
    // and only runInEditor objects update (photo mode, pause menus). Rendering,
    // UI and audio keep running.
    bool gamePaused = false;

    // ---- Loading screens -------------------------------------------------------
    // Shows `loadingScene` (a small scene the game makes, usually one object
    // drawing UI in onGui) until the world is ready:
    //   - load steps run on the main thread, one per frame, in order (e.g.
    //     "load the scene file"); the screen is redrawn between them;
    //   - load tasks report the progress of work that happens over many frames
    //     (foliage registers one while it fills its cells on worker threads).
    // Meanwhile the world updates like in the editor's Edit mode (only
    // runInEditor objects: terrain, foliage and grass stream, gameplay waits,
    // no physics) and isn't drawn: frames draw only the UI over clearColor.
    // Loading ends when every step ran, every task reports 1, and at least
    // minSeconds passed; the loading scene is then destroyed.
    //   engine.beginLoading(makeLoadingScene());
    //   engine.addLoadStep("Loading the savanna", [&] { scene.load(...); });
    void beginLoading(std::unique_ptr<Scene> loadingScene, float minSeconds = 0.0f,
                      const glm::vec4& clearColor = glm::vec4(0.02f, 0.02f, 0.025f, 1.0f));
    void addLoadStep(std::string name, std::function<void()> step);
    void addLoadTask(std::string name, std::function<float()> progress); // 0..1, 1 = done
    bool loading() const { return m_loadingScene != nullptr; }
    float loadProgress() const;                        // 0..1 over steps and tasks
    const std::string& loadStatus() const { return m_loadStatus; } // what's happening now
    Scene* loadingScene() { return m_loadingScene.get(); }
    bool loadStepsPending() const { return !m_loadSteps.empty(); }
    // Loading while the editor is in Edit mode: no loading scene is shown, the
    // editor keeps running (world paused, drawn as it streams in) and shows
    // the progress itself; loading ends as soon as steps and tasks are done.
    bool loadingInEditor() const;

private:
    std::vector<std::function<void()>> m_atShutdown;
    void pollEvents();
    void tickFrame();
    void tickLoading(); // a loading-screen frame
    void runLoadStep();
    bool pollLoadTasks(); // true when every step ran and every task is done
    void endLoading();
    void benchFrame(float cpuMs);

    // VE_BENCH=<frames>[,<warmup>]: average CPU/GPU timings over a run, log them, quit.
    struct Benchmark;
    std::unique_ptr<Benchmark> m_bench;

    EngineConfig m_config;
    Input m_input;
    Time m_time;
    std::unique_ptr<Window> m_window;
    std::unique_ptr<Renderer> m_renderer;
    std::unique_ptr<AudioSystem> m_audio;
    std::unique_ptr<PhysicsWorld> m_physics;
    std::unique_ptr<net::Network> m_net;
    std::unique_ptr<AssetManager> m_assets;
    std::unique_ptr<UiLayer> m_ui;
    std::unique_ptr<Editor> m_editor;
    std::unique_ptr<Scene> m_scene;
    std::unique_ptr<Scene> m_loadingScene;
    struct LoadStep {
        std::string name;
        std::function<void()> run;
    };
    struct LoadTask {
        std::string name;
        std::function<float()> progress;
        float last = 0.0f;
    };
    std::vector<LoadStep> m_loadSteps;
    std::vector<LoadTask> m_loadTasks;
    size_t m_loadStepsTotal = 0, m_loadStepsDone = 0;
    float m_loadMinSeconds = 0.0f, m_loadElapsed = 0.0f;
    std::string m_loadStatus;
    glm::vec4 m_loadClearColor{0.0f, 0.0f, 0.0f, 1.0f};
    bool m_running = false;
    double m_accumulator = 0.0;
    float m_fixedFrameDt = 0.0f; // VE_FIXED_DT (tests): fixed time per frame
};

} // namespace ve
