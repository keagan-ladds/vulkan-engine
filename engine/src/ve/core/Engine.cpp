#include "ve/core/Engine.hpp"

#include "ve/assets/AssetManager.hpp"
#include "ve/audio/Audio.hpp"
#include "ve/core/Log.hpp"
#include "ve/core/Window.hpp"
#include "ve/editor/Editor.hpp"
#include "ve/physics/Physics.hpp"
#include "ve/scene/Components.hpp"
#include "ve/scene/Reflection.hpp"
#include "ve/scene/Scene.hpp"
#include "ve/ui/UiLayer.hpp"
#include "ve/world/Vegetation.hpp"

#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace ve {

namespace {
Engine* g_engine = nullptr;
}

// Benchmark mode (VE_BENCH=<frames>[,<warmup>], usually with VE_HEADLESS=1):
// skips `warmup` frames (default 30: streaming, pipeline creation), then
// averages `frames` frames and logs one summary, then quits. CPU time is the
// whole frame minus the time spent waiting on GPU fences, so it measures the
// engine's own work; GPU times are the per-pass timestamps.
struct Engine::Benchmark {
    uint32_t warmup = 30, frames = 120, seen = 0;
    double cpuMs = 0, waitMs = 0, physicsMs = 0, gpuMs = 0, cpuMax = 0;
    std::vector<std::pair<std::string, double>> passMs;
};

Engine& Engine::get()
{
    return *g_engine;
}

bool Engine::exists()
{
    return g_engine != nullptr;
}

// EngineAccess shortcuts used by GameObject / Component.
Engine& EngineAccess::engine() { return Engine::get(); }
Input& EngineAccess::input() { return Engine::get().input(); }
const Time& EngineAccess::time() { return Engine::get().time(); }
Renderer& EngineAccess::renderer() { return Engine::get().renderer(); }
AssetManager& EngineAccess::assets() { return Engine::get().assets(); }
AudioSystem& EngineAccess::audio() { return Engine::get().audio(); }
PhysicsWorld& EngineAccess::physics() { return Engine::get().physics(); }

Engine::Engine(const EngineConfig& config) : m_config(config)
{
    if (g_engine) throw std::runtime_error("Only one ve::Engine may exist");
    g_engine = this;
    m_time.fixedDeltaTime = config.fixedTimestep;

    const auto exeDir = executableDirectory();
    auto resolve = [&](const std::filesystem::path& p) { return p.is_absolute() ? p : exeDir / p; };
    std::vector<std::filesystem::path> shaderDirs;
    for (const auto& p : config.shaderPaths) shaderDirs.push_back(resolve(p));
    shaderDirs.push_back(exeDir / "shaders");
    std::vector<std::filesystem::path> assetDirs;
    for (const auto& p : config.assetPaths) assetDirs.push_back(resolve(p));
    // Development builds: the game's source assets folder first (written by
    // ve_setup_game), so the editor reads and saves the real files.
    if (std::ifstream src(exeDir / "ve-source-assets.txt"); src) {
        std::string dir;
        std::getline(src, dir);
        if (!dir.empty() && std::filesystem::is_directory(dir)) assetDirs.insert(assetDirs.begin(), dir);
    }
    assetDirs.push_back(exeDir / "assets");

    RendererSettings rs = config.renderer;
    if (const char* h = std::getenv("VE_HEADLESS"); h && h[0] == '1') rs.headless = true;
    m_window = std::make_unique<Window>(config.title, config.width, config.height, rs.headless);
    m_renderer = std::make_unique<Renderer>();
    m_renderer->init(*m_window, rs, config.validation, shaderDirs);
    m_audio = std::make_unique<AudioSystem>();
    if (config.enableAudio) m_audio->init();
    m_physics = std::make_unique<PhysicsWorld>();
    m_physics->init();
    m_assets = std::make_unique<AssetManager>();
    m_assets->init(*m_renderer, assetDirs);
    m_ui = std::make_unique<UiLayer>();
    m_ui->init(*this);
    m_scene = std::make_unique<Scene>();
    registerBuiltinTypes(); // reflection: scene files + editor
    if (config.enableEditor) {
        m_editor = std::make_unique<Editor>();
        m_editor->init(*this);
    }
    if (const char* b = std::getenv("VE_BENCH"); b && *b) {
        m_bench = std::make_unique<Benchmark>();
        unsigned frames = 120, warmup = 30;
        std::sscanf(b, "%u,%u", &frames, &warmup);
        m_bench->frames = std::max(1u, frames);
        m_bench->warmup = warmup;
    }
    log::info("Engine ready");
}

void Engine::benchFrame(float cpuMs)
{
    Benchmark& b = *m_bench;
    if (b.seen++ < b.warmup) return;
    const float wait = m_renderer->lastFenceWaitMs();
    const double cpu = std::max(0.0f, cpuMs - wait);
    b.cpuMs += cpu;
    b.cpuMax = std::max(b.cpuMax, cpu);
    b.waitMs += wait;
    b.physicsMs += m_physics->lastStepMs;
    b.gpuMs += m_renderer->lastGpuFrameTimeMs();
    for (const auto& p : m_renderer->graph().passes()) {
        if (!p->enabled) continue;
        auto it = std::find_if(b.passMs.begin(), b.passMs.end(), [&](auto& e) { return e.first == p->name(); });
        if (it == b.passMs.end()) it = b.passMs.insert(b.passMs.end(), {p->name(), 0.0});
        it->second += p->lastGpuTimeMs;
    }
    const uint32_t n = b.seen - b.warmup;
    if (n < b.frames) return;
    log::info("[bench] {} frames: cpu {:.3f} ms (max {:.3f}), gpu wait {:.3f} ms, physics step {:.3f} ms, gpu {:.3f} ms",
              n, b.cpuMs / n, b.cpuMax, b.waitMs / n, b.physicsMs / n, b.gpuMs / n);
    for (const auto& [name, ms] : b.passMs) log::info("[bench]   {:<28} {:8.3f} ms", name, ms / n);
    quit();
}

Engine::~Engine()
{
    // Tear down in reverse dependency order. Scene objects hold meshes,
    // materials, textures and sounds, so they go first.
    if (m_renderer) m_renderer->device().waitIdle();
    if (m_editor) m_editor->shutdown();
    m_editor.reset();
    m_scene.reset();
    MeshLibrary::releaseMeshes();
    m_assets.reset();
    if (m_ui) m_ui->shutdown(); // no-op if run() already did it
    m_ui.reset();
    m_audio.reset();
    m_physics.reset(); // after the scene: Rigidbodies remove themselves from the world
    m_renderer.reset();
    m_window.reset();
    g_engine = nullptr;
}

void Engine::quit()
{
    m_running = false;
}

void Engine::pollEvents()
{
    m_input.beginFrame();
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        m_ui->processEvent(e);
        m_input.processEvent(e);
        m_window->processEvent(e);
    }
    if (m_window->consumeResized()) m_renderer->onResize();
    if (m_window->shouldClose()) m_running = false;
}

void Engine::run()
{
    using Clock = std::chrono::steady_clock;
    m_running = true;
    auto last = Clock::now();
    const auto startTime = last;

    while (m_running) {
        pollEvents();
        if (!m_running) break;
        if (m_window->isMinimized()) {
            m_window->waitForEvent();
            last = Clock::now(); // don't count time spent minimized
            continue;
        }

        const auto now = Clock::now();
        float realDt = std::chrono::duration<float>(now - last).count();
        last = now;
        realDt = std::min(realDt, 0.25f); // clamp hitches (debugger breaks, loading)

        m_time.unscaledDeltaTime = realDt;
        m_time.deltaTime = realDt * m_time.timeScale;
        m_time.time += m_time.deltaTime;
        m_time.realTime = std::chrono::duration<float>(now - startTime).count();
        m_time.fixedDeltaTime = m_config.fixedTimestep;
        tickFrame();
        if (m_bench) benchFrame(std::chrono::duration<float, std::milli>(Clock::now() - now).count());
        ++m_time.frameCount;
    }

    m_renderer->device().waitIdle();
    m_ui->shutdown();
}

void Engine::tickFrame()
{
    m_ui->beginFrame();
    m_input.setUiCapture(m_ui->wantsKeyboard(), m_ui->wantsMouse());

    if (m_editor) m_editor->beginFrame(m_time.unscaledDeltaTime);
    // Editor "Edit" mode: the game is paused; only runInEditor objects update.
    const bool paused = (m_editor && m_editor->paused()) || gamePaused;

    m_scene->startPending();

    // Fixed timestep: run as many fixed steps as the elapsed time requires.
    if (!paused) m_accumulator += m_time.deltaTime;
    int steps = 0;
    // Each step: gameplay fixedUpdate() hooks (forces, character movement), then the physics world.
    while (m_accumulator >= m_time.fixedDeltaTime && steps < m_config.maxFixedStepsPerFrame) {
        m_scene->fixedUpdate(m_time.fixedDeltaTime);
        const auto t0 = std::chrono::steady_clock::now();
        m_physics->step(*m_scene, m_time.fixedDeltaTime);
        m_physics->lastStepMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
        m_accumulator -= m_time.fixedDeltaTime;
        ++steps;
    }
    if (steps == m_config.maxFixedStepsPerFrame) m_accumulator = 0.0; // drop time we can't catch up on
    m_time.fixedAlpha = static_cast<float>(m_accumulator / m_time.fixedDeltaTime);
    // Smooth motion: place physics objects between their last two fixed-step poses.
    if (!paused) m_physics->interpolate(*m_scene, m_time.fixedAlpha);

    m_scene->update(m_time.deltaTime, paused);
    m_scene->lateUpdate(m_time.deltaTime, paused);
    m_scene->onGui();
    if (m_editor) m_editor->onGui();
    m_ui->drawDebugOverlay();

    if (Camera* cam = m_scene->mainCamera()) {
        const Transform& t = cam->transform();
        m_audio->setListener(t.position(), t.forward(), t.up());
    }
    m_audio->update(m_time.unscaledDeltaTime);

    m_renderer->render(*m_scene, m_time);
    m_ui->endFrameIfNotRendered();
    m_scene->flushDestroyed();
}

} // namespace ve
