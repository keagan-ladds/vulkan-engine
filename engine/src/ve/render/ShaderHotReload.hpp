#pragma once

// =============================================================================
// ShaderHotReload - recompiles GLSL shaders whose sources changed while the
// app runs, so the renderer can rebuild their pipelines.
//
// The build (cmake/Shaders.cmake) leaves two files next to each
// <name>.spv: <name>.spv.cmd (the exact glslc command, one argument per line)
// and <name>.spv.d (glslc's depfile: the source plus every #include). Every
// half second the watcher checks those sources' modification times; changed
// shaders are recompiled with the same command (in parallel), and the names of
// the ones that compiled are handed to Renderer::reloadShaders().
//
// A compile error is logged and the old .spv stays in use. Only dev builds
// have the .cmd files, so a shipped game watches nothing. Set
// VE_SHADER_HOT_RELOAD=0 to turn it off.
// =============================================================================

#include <chrono>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace ve {

class ShaderHotReload {
public:
    explicit ShaderHotReload(const std::vector<std::filesystem::path>& shaderDirectories);

    size_t watchedCount() const { return m_entries.size(); }
    // Recompiles shaders whose sources changed (throttled; call every frame).
    // Returns the names (e.g. "bloom_up.frag") that compiled successfully.
    std::vector<std::string> poll();
    // Checks right now, ignoring the throttle.
    std::vector<std::string> pollNow();

    const std::string& lastError() const { return m_lastError; }
    uint32_t reloadCount() const { return m_reloads; }

private:
    struct Entry {
        std::string name; // "bloom_up.frag"
        std::filesystem::path depfile;
        std::vector<std::string> command;
        std::vector<std::filesystem::path> sources;
        std::filesystem::file_time_type stamp{};
    };

    void readDepfile(Entry& e);
    // Newest modification time of e's sources. `seen` caches stat() results
    // within one poll: shared includes (ve.glsl) are checked once, not per shader.
    using StampCache = std::unordered_map<std::string, std::filesystem::file_time_type>;
    static std::filesystem::file_time_type newest(const Entry& e, StampCache& seen);

    std::vector<Entry> m_entries;
    std::chrono::steady_clock::time_point m_lastPoll{};
    std::string m_lastError;
    uint32_t m_reloads = 0;
};

} // namespace ve
