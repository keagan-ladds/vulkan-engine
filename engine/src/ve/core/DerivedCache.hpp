#pragma once

// =============================================================================
// DerivedCache - data the engine computes from assets (simplified LOD meshes,
// impostor atlases...), kept on disk keyed by a hash of everything it was made
// from, so it's computed once instead of on every launch.
//
//   const uint64_t key = derived::Hasher().add(meshBytes).add(options).value();
//   const auto file = derived::path("lod", key, ".mesh");
//   if (auto bytes = vfs::read(file)) { ...use it...; derived::noteHit(); }
//   else { ...compute...; derived::writeFile(file, data); derived::noteMiss(); }
//
// Files live in <cook cache>/derived/<kind>/<key hex><ext> (AssetManager sets
// the folder from EngineConfig::cookCache). `vecook --bake` fills it by
// running the game headless (VE_BAKE=1), and `vecook --package` ships it, so
// a packaged game reads everything and computes nothing. Keys must only
// depend on content (bytes, settings, asset names), never on paths or
// pointers, so they match between machines. Thread-safe.
// =============================================================================

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <type_traits>

namespace ve::derived {

/// 64-bit FNV-1a over everything added (in order). Deterministic across runs and machines.
class Hasher {
public:
    /// Adds raw bytes.
    Hasher& add(const void* data, size_t size);
    /// Adds a string's characters (and its length, so "ab"+"c" != "a"+"bc").
    Hasher& add(std::string_view text);
    /// Adds a plain value's bytes (numbers, glm vectors, POD structs without padding).
    template <class T>
        requires std::is_trivially_copyable_v<T>
    Hasher& value(const T& v)
    {
        return add(&v, sizeof(T));
    }
    /// The hash so far.
    uint64_t value() const { return m_hash; }

private:
    uint64_t m_hash = 14695981039346656037ull;
};

/// Sets the cache folder (AssetManager does, from the cook cache: <cache>/derived). Empty = off.
void setDirectory(const std::filesystem::path& directory);
/// The cache folder (empty when off).
const std::filesystem::path& directory();
/// Where the entry `key` of `kind` lives: <directory>/<kind>/<16 hex digits><extension>.
/// Empty when the cache is off.
std::filesystem::path path(std::string_view kind, uint64_t key, std::string_view extension);
/// Writes a cache file (via a temporary file + rename: readers never see half
/// a file). False if the cache is off or the write failed (logged once).
bool writeFile(const std::filesystem::path& file, const void* data, size_t size);

/// Counters for logs, tests and bake mode.
struct Stats {
    std::atomic<uint32_t> hits{0};    ///< entries found
    std::atomic<uint32_t> misses{0};  ///< entries computed (not found)
    std::atomic<uint32_t> writes{0};  ///< entries written
    std::atomic<int32_t> pending{0};  ///< writes queued on worker threads, not finished yet
};
/// The counters (process-wide).
Stats& stats();
inline void noteHit() { ++stats().hits; }     ///< Count a cache hit.
inline void noteMiss() { ++stats().misses; }  ///< Count an entry that had to be computed.

/// True in bake mode (env VE_BAKE=1, `vecook --bake`): the game runs to fill
/// the cache, and the Engine quits once loading is done and writes finished.
bool bakeMode();

} // namespace ve::derived
