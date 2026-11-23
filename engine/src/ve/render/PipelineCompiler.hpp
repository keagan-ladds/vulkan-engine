#pragma once

// =============================================================================
// PipelineCompiler - mesh pipelines built on worker threads.
//
// The driver turns SPIR-V into GPU code inside vkCreateGraphicsPipelines.
// That takes tens to hundreds of milliseconds per pipeline on some drivers
// (NVIDIA's especially; longer still with validation layers), and building
// them while recording a frame froze the game the first time something with
// a new shader (or the first shadow-casting point light) appeared. Now:
//
//   - Renderer::createShader starts every pipeline the shader can need right
//     away (forward, plus the shadow kinds if it casts shadows);
//   - Shader::pipeline() returns VK_NULL_HANDLE until its pipeline is ready
//     and draw code skips that draw (the object shows up a few frames later
//     instead of the whole frame stalling);
//   - loading screens wait for outstanding compiles ("Compiling shaders");
//   - the pipeline cache is saved about a second after a burst of compiles,
//     not only at a clean shutdown, so a crash or a killed process keeps them.
//
// Headless runs (tests, captures) and RendererSettings::asyncPipelines = false
// build on the calling thread, as before.
//
// Thread safety: a job only calls Vulkan with inputs copied on the main thread
// (vkCreateShaderModule / vkCreateGraphicsPipelines are free-threaded and the
// VkPipelineCache is internally synchronized). It never touches the Renderer,
// the Device object or any Shader, as Jobs.hpp requires.
// =============================================================================

#include "ve/core/Jobs.hpp"
#include "ve/render/Material.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ve {

class Device;

// Everything a mesh pipeline build needs, resolved on the main thread.
struct MeshPipelineInputs {
    VkDevice device = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    ShaderDesc desc;
    PassKind kind = PassKind::Forward;
    std::filesystem::path vertex;
    std::filesystem::path fragment; // empty = depth only
    bool fillModeNonSolid = false;
    bool depthClamp = false;
};

// Builds a mesh pipeline. Safe on any thread; throws on failure.
VkPipeline buildMeshPipeline(const MeshPipelineInputs& in);

// One pipeline being built. The job writes `pipeline` / `error`; the main
// thread reads them only once job.done().
struct PipelineCompile {
    jobs::Handle job;
    VkPipeline pipeline = VK_NULL_HANDLE;
    std::string error;
    float ms = 0.0f;        // build time (log)
    bool claimed = false;   // its Shader took the pipeline (main thread)
    bool abandoned = false; // its Shader no longer wants it: destroyed when done (main thread)
};

class PipelineCompiler {
public:
    void init(Device& device) { m_device = &device; }

    // Starts building on a worker thread.
    std::shared_ptr<PipelineCompile> submit(MeshPipelineInputs inputs);

    // Main thread, once a frame: frees abandoned results, logs a summary of
    // each burst of compiles, saves the pipeline cache after a burst.
    void poll();
    // Shutdown: waits for every job and frees results nobody claimed. Later
    // submits build nothing.
    void shutdown();

    size_t pending() const;                          // submitted, not finished
    uint64_t submittedCount() const { return m_submitted; }
    uint64_t finishedCount() const;                  // includes jobs that finished since the last poll()

private:
    using Clock = std::chrono::steady_clock;
    Device* m_device = nullptr;
    std::vector<std::shared_ptr<PipelineCompile>> m_inFlight;
    uint64_t m_submitted = 0;
    uint64_t m_finished = 0;
    // The current burst (log summary) and the cache save that follows it.
    uint32_t m_burstCount = 0;
    float m_burstSlowestMs = 0.0f;
    Clock::time_point m_burstStart{};
    Clock::time_point m_lastFinish{};
    bool m_cacheDirty = false;
    bool m_shutdown = false;
};

} // namespace ve
