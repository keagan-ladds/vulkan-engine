#include "ve/core/Engine.hpp"

#include "ve/assets/AssetManager.hpp"
#include "ve/audio/Audio.hpp"
#include "ve/core/Log.hpp"
#include "ve/core/Window.hpp"
#include "ve/physics/Physics.hpp"
#include "ve/scene/Components.hpp"
#include "ve/scene/Scene.hpp"
#include "ve/ui/UiLayer.hpp"

#include <SDL.h>

#include <algorithm>
#include <chrono>

namespace ve {

namespace {
Engine* g_engine = nullptr;
}

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
    assetDirs.push_back(exeDir / "assets");

    m_window = std::make_unique<Window>(config.title, config.width, config.height);
    m_renderer = std::make_unique<Renderer>();
    m_renderer->init(*m_window, config.renderer, config.validation, shaderDirs);
    m_audio = std::make_unique<AudioSystem>();
    if (config.enableAudio) m_audio->init();
    m_physics = std::make_unique<PhysicsWorld>();
    m_physics->init();
    m_assets = std::make_unique<AssetManager>();
    m_assets->init(*m_renderer, assetDirs);
    m_ui = std::make_unique<UiLayer>();
    m_ui->init(*this);
    m_scene = std::make_unique<Scene>();
    log::info("Engine ready");
}

Engine::~Engine()
{
    // Tear down in reverse dependency order. Scene objects hold meshes,
    // materials, textures and sounds, so they go first.
    if (m_renderer) m_renderer->device().waitIdle();
    m_scene.reset();
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
        ++m_time.frameCount;
    }

    m_renderer->device().waitIdle();
    m_ui->shutdown();
}

void Engine::tickFrame()
{
    m_ui->beginFrame();
    m_input.setUiCapture(m_ui->wantsKeyboard(), m_ui->wantsMouse());

    m_scene->startPending();

    // Fixed timestep: run as many fixed steps as the elapsed time requires.
    m_accumulator += m_time.deltaTime;
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
    m_physics->interpolate(*m_scene, m_time.fixedAlpha);

    m_scene->update(m_time.deltaTime);
    m_scene->lateUpdate(m_time.deltaTime);
    m_scene->onGui();
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
