#include "ve/core/Engine.hpp"
#include "ve/assets/DataAsset.hpp"
#include "ve/assets/MaterialAsset.hpp"
#include "ve/render/Renderer.hpp"
#include "ve/net/Network.hpp"
#include "ve/debug/Debug.hpp"

#include "ve/anim/Animation.hpp"
#include "ve/assets/AssetManager.hpp"
#include "ve/audio/Audio.hpp"
#include "ve/core/Log.hpp"
#include "ve/core/Profiler.hpp"
#include "ve/ui/Gui.hpp"
#include "ve/core/Window.hpp"
#include "ve/editor/Editor.hpp"
#include "ve/gameplay/Interaction.hpp"
#include "ve/gameplay/Placement.hpp"
#include "ve/gameplay/ProxySet.hpp"
#include "ve/nav/Navigation.hpp"
#include "ve/physics/Physics.hpp"
#include "ve/scene/Components.hpp"
#include "ve/scene/Reflection.hpp"
#include "ve/scene/Scene.hpp"
#include "ve/ui/UiLayer.hpp"
#include "ve/world/Terrain.hpp"
#include "ve/world/Vegetation.hpp"

#include <SDL.h>

#include <algorithm>
#include <thread>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <fstream>

#ifdef _WIN32
#include <windows.h> // GetThreadTimes (NOMINMAX / WIN32_LEAN_AND_MEAN: set by CMake)
#endif

namespace ve {

namespace {
Engine* g_engine = nullptr;

// Reflection: every module's components (scene files, editor). Each module
// registers its own types, so the scene module doesn't include the modules
// above it; this is the one place that knows them all. Once per process (the
// registry is global and outlives an Engine).
void registerAllTypes()
{
    static bool done = false;
    if (done) return;
    done = true;
    registerBuiltinTypes(); // scene: GameObject, Camera, lights, ModelInstance, ...
    TypeRegistry& r = TypeRegistry::get();
    registerNetworkTypes(r);
    registerInteractionTypes(r);
    registerPlacementTypes(r);
    registerProxyTypes(r);
    registerNavigationTypes(r);
    registerAnimationTypes(r);
    registerPhysicsTypes(r);
    registerAudioTypes(r);
    registerTerrainTypes(r);
    registerVegetationTypes(r);
    registerAssetTypes(r); // MaterialAsset (.material files)
}
} // namespace

// Benchmark mode (VE_BENCH=<frames>[,<warmup>], usually with VE_HEADLESS=1):
// skips `warmup` frames (default 30: streaming, pipeline creation), then
// averages `frames` frames and logs one summary, then quits.
// - frame: wall time minus GPU fence waits and the driver's submit/present
// - main thread: CPU time the main thread actually consumed (the engine's
//   own cost, unaffected by other threads, e.g. a software GPU driver)
// - gpu: the per-pass timestamps.
// CPU time consumed by the calling thread (not wall time): on a software
// Vulkan driver the GPU work runs on the same cores and would otherwise be
// counted as engine time through preemption.
double threadCpuMs()
{
#if defined(__linux__) || defined(__APPLE__)
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return double(ts.tv_sec) * 1e3 + double(ts.tv_nsec) * 1e-6;
#elif defined(_WIN32)
    FILETIME created, exited, kernel, user; // 100 ns units
    if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
        auto ticks = [](const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
        return double(ticks(kernel) + ticks(user)) * 1e-4;
    }
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
#else
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

struct Engine::Benchmark {
    uint32_t warmup = 30, frames = 120, seen = 0;
    double cpuMs = 0, threadMs = 0, waitMs = 0, submitMs = 0, physicsMs = 0, gpuMs = 0, cpuMax = 0;
    double frameStart = 0; // main-thread CPU time at the start of the frame
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
net::Network& EngineAccess::net() { return Engine::get().net(); }

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
    // VE_FIXED_DT=0.016667: every frame advances time by this much, however long it
    // took (headless tests on slow software rendering see real-time motion per frame).
    if (const char* f = std::getenv("VE_FIXED_DT"); f && *f) m_fixedFrameDt = std::strtof(f, nullptr);
    m_window = std::make_unique<Window>(config.title, config.width, config.height, rs.headless);
    m_renderer = std::make_unique<Renderer>();
    m_renderer->init(*m_window, rs, config.validation, shaderDirs);
    registerSkinnedMeshes(m_renderer->skinning()); // the renderer's skinning pass finds anim's SkinnedMeshRenderers
    m_audio = std::make_unique<AudioSystem>();
    if (config.enableAudio) m_audio->init();
    m_physics = std::make_unique<PhysicsWorld>();
    m_net = std::make_unique<net::Network>(*this);
    m_physics->init();
    m_assets = std::make_unique<AssetManager>();
    m_assets->init(*m_renderer, assetDirs);
    {
        AssetManager::CookOptions cook;
        cook.cacheDir = config.cookCache.empty() ? exeDir / "cooked" : config.cookCache.is_absolute() ? config.cookCache : exeDir / config.cookCache;
        if (const char* d = std::getenv("VE_COOK_DIR"); d && *d) cook.cacheDir = d;
        cook.cookOnDemand = config.cookOnDemand;
        const std::string mode = std::getenv("VE_COOK") ? std::getenv("VE_COOK") : "";
        if (mode == "0" || mode == "off") cook.useCooked = cook.cookOnDemand = false;
        else if (mode == "read") cook.cookOnDemand = false;
        else if (mode == "sync") cook.synchronous = cook.cookOnDemand = true;
        m_assets->setCooking(cook);
    }
    m_ui = std::make_unique<UiLayer>();
    m_ui->init(*this);
    m_scene = std::make_unique<Scene>();
    registerAllTypes(); // reflection: scene files + editor
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
    const float submit = m_renderer->lastSubmitMs();
    const double cpu = std::max(0.0f, cpuMs - wait - submit);
    b.cpuMs += cpu;
    b.threadMs += threadCpuMs() - b.frameStart;
    b.cpuMax = std::max(b.cpuMax, cpu);
    b.waitMs += wait;
    b.submitMs += submit;
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
    log::info("[bench] {} frames: main thread {:.3f} ms, frame {:.3f} ms (max {:.3f}), gpu wait {:.3f} ms, submit {:.3f} ms, "
              "physics step {:.3f} ms, gpu {:.3f} ms",
              n, b.threadMs / n, b.cpuMs / n, b.cpuMax, b.waitMs / n, b.submitMs / n, b.physicsMs / n, b.gpuMs / n);
    const InstanceStats& is = m_renderer->instances().stats();
    log::info("[bench] instances {} (camera draws {}, shadow draws {})", is.instances, is.visibleCamera, is.visibleShadow);
    const RenderStats& rs = m_renderer->stats();
    log::info("[bench] draw calls {} (shadow {}, indirect {}), objects {} ({} batched static)", rs.drawCalls, rs.shadowDrawCalls,
              is.drawCalls, rs.totalObjects, rs.staticObjects);
    for (const auto& [name, ms] : b.passMs) log::info("[bench]   {:<28} {:8.3f} ms", name, ms / n);
    for (const Profiler::Zone& z : Profiler::get().zones())
        log::info("[bench] cpu {}{:<28} {:8.3f} ms", std::string(size_t(z.depth) * 2, ' '), z.name, z.avgMs);
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
    for (auto& fn : m_atShutdown) fn(); // modules' static GPU caches (e.g. world's MeshLibrary)
    m_atShutdown.clear();
    m_assets.reset();
    if (m_ui) m_ui->shutdown(); // no-op if run() already did it
    m_ui.reset();
    m_audio.reset();
    m_net.reset();     // (after the scene: components ask it whether they're online)
    m_physics.reset(); // after the scene: Rigidbodies remove themselves from the world
    m_renderer.reset();
    m_window.reset();
    // Last: everything that submits jobs (world systems, grass passes, shader
    // hot-reload) is gone. Queued jobs are dropped, running ones finish.
    jobs::shutdown();
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
        if (m_fixedFrameDt > 0.0f) realDt = m_fixedFrameDt; // VE_FIXED_DT: simulate a steady frame rate (tests)

        m_time.unscaledDeltaTime = realDt;
        m_time.deltaTime = realDt * m_time.timeScale;
        m_time.time += m_time.deltaTime;
        m_time.realTime = std::chrono::duration<float>(now - startTime).count();
        m_time.fixedDeltaTime = m_config.fixedTimestep;
        const bool loadingFrame = loading();
        if (m_bench) m_bench->frameStart = threadCpuMs();
        Profiler::get().beginFrame();
        tickFrame();
        Profiler::get().endFrame();
        if (m_bench && !loadingFrame) benchFrame(std::chrono::duration<float, std::milli>(Clock::now() - now).count());
        ++m_time.frameCount;
    }

    m_renderer->device().waitIdle();
    m_ui->shutdown();
}

void Engine::beginLoading(std::unique_ptr<Scene> loadingScene, float minSeconds, const glm::vec4& clearColor)
{
    m_loadingScene = loadingScene ? std::move(loadingScene) : std::make_unique<Scene>();
    m_loadMinSeconds = minSeconds;
    m_loadElapsed = 0.0f;
    m_loadSteps.clear();
    m_loadTasks.clear();
    m_loadStepsTotal = m_loadStepsDone = 0;
    m_loadStatus = "Loading";
    m_loadClearColor = clearColor;
    m_renderer->setLoadingFrames(!loadingInEditor(), clearColor);
}

void Engine::addLoadStep(std::string name, std::function<void()> step)
{
    if (!loading()) {
        step(); // nothing to show progress on: just do it
        return;
    }
    m_loadSteps.push_back({std::move(name), std::move(step)});
    ++m_loadStepsTotal;
}

void Engine::addLoadTask(std::string name, std::function<float()> progress)
{
    if (loading()) m_loadTasks.push_back({std::move(name), std::move(progress), 0.0f});
}

float Engine::loadProgress() const
{
    // Steps and tasks weigh the same; each task counts with its own progress.
    const size_t parts = m_loadStepsTotal + m_loadTasks.size();
    if (parts == 0) return loading() ? 0.0f : 1.0f;
    float done = float(m_loadStepsDone);
    for (const LoadTask& t : m_loadTasks) done += t.last;
    return std::clamp(done / float(parts), 0.0f, 1.0f);
}

void Engine::endLoading()
{
    m_loadingScene.reset();
    m_loadTasks.clear();
    m_loadSteps.clear();
    m_renderer->setLoadingFrames(false);
    m_accumulator = 0.0; // no burst of fixed steps for the time spent loading
    Profiler::get().reset(); // loading frames have different zones and timings
    log::info("Loading done in {:.1f} s", m_loadElapsed);
}

void Engine::runLoadStep()
{
    // One main-thread step per frame (the screen is redrawn in between).
    if (m_loadSteps.empty()) return;
    LoadStep step = std::move(m_loadSteps.front());
    m_loadSteps.erase(m_loadSteps.begin());
    m_loadStatus = step.name;
    const auto t0 = std::chrono::steady_clock::now();
    step.run();
    log::info("Loading: '{}' took {:.2f} s", step.name, std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count());
    ++m_loadStepsDone;
}

bool Engine::pollLoadTasks()
{
    bool tasksDone = true;
    for (LoadTask& t : m_loadTasks) {
        const float before = t.last;
        t.last = std::clamp(t.progress ? t.progress() : 1.0f, 0.0f, 1.0f);
        if (t.last >= 1.0f && before < 1.0f) log::info("Loading: '{}' done at {:.1f} s", t.name, m_loadElapsed);
        if (t.last < 1.0f) {
            if (tasksDone && m_loadSteps.empty()) m_loadStatus = t.name;
            tasksDone = false;
        }
    }
    if (!m_loadSteps.empty()) m_loadStatus = m_loadSteps.front().name;
    return tasksDone && m_loadSteps.empty();
}

bool Engine::editorPaused() const
{
    return m_editor && m_editor->paused();
}

bool Engine::loadingInEditor() const
{
    return m_loadingScene && m_editor && m_editor->editing();
}

void Engine::tickLoading()
{
    const auto frameStart = std::chrono::steady_clock::now();
    m_renderer->setLoadingFrames(true, m_loadClearColor);
    m_ui->beginFrame();
    m_input.setUiCapture(m_ui->wantsKeyboard(), m_ui->wantsMouse());
    m_loadElapsed += m_time.unscaledDeltaTime;

    runLoadStep();
    // The world streams (runInEditor objects only), gameplay waits.
    m_scene->startPending();
    m_scene->update(m_time.deltaTime, true);
    m_scene->lateUpdate(m_time.deltaTime, true);
    const bool done = pollLoadTasks();

    m_loadingScene->startPending();
    m_loadingScene->update(m_time.unscaledDeltaTime);
    m_loadingScene->lateUpdate(m_time.unscaledDeltaTime);
    m_loadingScene->onGui();
    m_renderer->render(*m_loadingScene, m_time);
    m_ui->endFrameIfNotRendered();
    m_loadingScene->flushDestroyed();
    m_scene->flushDestroyed();

    if (done && m_loadElapsed >= m_loadMinSeconds) {
        endLoading();
        return;
    }
    // A loading screen needs no more than ~30 Hz: the cores are better spent
    // on the loading workers (software renderers and weak CPUs especially).
    const auto frameTime = std::chrono::steady_clock::now() - frameStart;
    if (frameTime < std::chrono::milliseconds(33)) std::this_thread::sleep_for(std::chrono::milliseconds(33) - frameTime);
}

void Engine::tickFrame()
{
    m_net->poll(); // (also while loading: keeps the connection alive; messages wait for the load)
    // Loading while the editor is open: no game loading screen. The editor
    // stays usable and shows progress while the world fills in around it.
    const bool editorLoading = loadingInEditor();
    if (m_loadingScene && !editorLoading) {
        tickLoading();
        return;
    }
    if (editorLoading) {
        m_renderer->setLoadingFrames(false);
        m_loadElapsed += m_time.unscaledDeltaTime;
        runLoadStep();
    }
    m_ui->beginFrame();
    ui::System::get().route(*this); // game UI first: what it takes, gameplay doesn't see (+ ImGui's capture)

    if (m_editor) m_editor->beginFrame(m_time.unscaledDeltaTime);
    // Editor "Edit" mode: the game is paused; only runInEditor objects update.
    const bool paused = editorPaused() || gamePaused || editorLoading;

    m_scene->startPending();

    // Fixed timestep: run as many fixed steps as the elapsed time requires.
    if (!paused) m_accumulator += m_time.deltaTime;
    int steps = 0;
    // Each step: gameplay fixedUpdate() hooks (forces, character movement), then the physics world.
    {
        VE_PROFILE("Fixed update + physics");
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
    }

    DataAssets::poll(m_time.unscaledDeltaTime); // biome / foliage / grass / material files edited on disk
    m_assets->updateMaterials();                // (material files apply their changes in place)
    m_assets->updateCooking();                  // finished background texture cooks
    {
        VE_PROFILE("Update");
        m_scene->update(m_time.deltaTime, paused);
    }
    {
        VE_PROFILE("Late update");
        m_scene->lateUpdate(m_time.deltaTime, paused);
    }
    {
        VE_PROFILE("Network");
        m_net->flush(); // what we own moved / changed this frame
    }
    debug::drawEngineChannels(); // colliders, navmesh, agents (when their F1 channels are on)
    if (editorLoading && pollLoadTasks()) endLoading(); // (no minimum time: there's no screen to read)
    {
        VE_PROFILE("GUI");
        m_scene->onGui();
        debug::drawLabels();            // ve::debug::text, under everything else
        ui::System::get().draw(*this); // game UI canvases (under the tools' ImGui windows)
        if (m_editor) m_editor->onGui();
        m_ui->drawDebugOverlay();
    }

    if (Camera* cam = m_scene->mainCamera()) {
        const Transform& t = cam->transform();
        m_audio->setListener(t.position(), t.forward(), t.up());
    }
    {
        VE_PROFILE("Audio");
        m_audio->update(m_time.unscaledDeltaTime);
    }
    {
        VE_PROFILE("Render");
        m_renderer->render(*m_scene, m_time);
    }
    debug::endFrame(m_time.realTime); // one-frame lines are drawn: drop them
    m_ui->endFrameIfNotRendered();
    m_scene->flushDestroyed();
}

} // namespace ve
