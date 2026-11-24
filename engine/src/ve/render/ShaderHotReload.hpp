#pragma once

// =============================================================================
// ShaderHotReload - recompiles GLSL shaders whose sources changed while the
// app runs, so the renderer can rebuild their pipelines.
//
// The build (cmake/Shaders.cmake) leaves two files next to each
// <name>.spv: <name>.spv.cmd (the exact glslc command, one argument per line)
// and <name>.spv.d (glslc's depfile: the source plus every #include). Every
// half second the watcher checks those sources' modification times; changed
// shaders are recompiled with the same command (in parallel, in the
// background: the frame doesn't wait), and the names of the ones that compiled
// are handed to Renderer::reloadShaders() on the first poll after they finish.
//
// A compile error is logged and the old .spv stays in use. Only dev builds
// have the .cmd files, so a shipped game watches nothing. Set
// VE_SHADER_HOT_RELOAD=0 to turn it off.
// =============================================================================

#include "ve/core/Jobs.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ve {

/// Recompiles edited GLSL with the build's own glslc commands, in the background (Renderer::shaderHotReload()).
class ShaderHotReload {
public:
    /// Watches the shaders whose compile commands the build left in these folders.
    explicit ShaderHotReload(const std::vector<std::filesystem::path>& shaderDirectories);

    /// Number of shaders watched.
    size_t watchedCount() const { return m_entries.size(); }
    /// Starts recompiling shaders whose sources changed (throttled; call every
    /// frame) without waiting. Returns the names (e.g. "bloom_up.frag") of
    /// earlier compiles that have finished successfully since the last call.
    std::vector<std::string> poll();
    /// Checks right now, ignoring the throttle, and waits for every compile.
    std::vector<std::string> pollNow();

    /// The last compile error ("" after a successful round).
    const std::string& lastError() const { return m_lastError; }
    /// Compiles running now.
    size_t compiling() const { return m_jobs.size(); }
    /// Called when a recompile starts (ok and milliseconds unused) and ends.
    std::function<void(const std::string& name, bool started, bool ok, float milliseconds, const std::string& output)> onCompile;
    /// Successful reload rounds so far.
    uint32_t reloadCount() const { return m_reloads; }

private:
    struct Entry {
        std::string name; ///< "bloom_up.frag"
        std::filesystem::path depfile;
        std::vector<std::string> command;
        std::vector<std::filesystem::path> sources;
        std::filesystem::file_time_type stamp{};
    };

    struct CompileResult {
        bool ok = false;
        std::string output;
    };
    struct Job {
        Entry* entry;
        jobs::Handle job; ///< a background pool job running glslc
        std::shared_ptr<CompileResult> result;
        std::chrono::steady_clock::time_point started;
    };
    static CompileResult runCommand(const std::vector<std::string>& args);
    void startChanged();                                  ///< launches compiles of changed, idle entries
    std::vector<std::string> finishJobs(bool wait);       ///< collects finished (or all) compiles

    void readDepfile(Entry& e);
    /// Newest modification time of e's sources. `seen` caches stat() results
    /// within one poll: shared includes (ve.glsl) are checked once, not per shader.
    using StampCache = std::unordered_map<std::string, std::filesystem::file_time_type>;
    static std::filesystem::file_time_type newest(const Entry& e, StampCache& seen);

    std::vector<Entry> m_entries; ///< fixed after construction (jobs point into it)
    std::vector<Job> m_jobs;      ///< compiles in flight
    std::chrono::steady_clock::time_point m_lastPoll{};
    std::string m_lastError;
    uint32_t m_reloads = 0;
};

} // namespace ve
