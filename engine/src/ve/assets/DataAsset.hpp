#pragma once

// =============================================================================
// DataAssets - small JSON asset files that scene objects reference by path
// (terrain biomes "*.biome", foliage "*.foliage", grass "*.grass"...), shared
// by everything that references them and live while the game runs:
//
//   const auto biome = ve::DataAssets::get("biomes/savannah.biome"); // cached JSON (null if missing)
//   if (ve::DataAssets::generation() != m_seen) { ...re-read what you use... }
//
//   - Files are found like other assets (the source assets folder first in a
//     development build, then the asset search paths).
//   - A file edited on disk is reloaded within half a second (Engine polls),
//     and generation() bumps: users re-read and rebuild.
//   - The editor edits the cached copy (set(): live, unsaved) and writes it
//     back with save() (to the source assets folder in a development build).
//
// Main thread only.
// =============================================================================

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ve {

using Json = nlohmann::json;

class DataAssets {
public:
    // The file's JSON (cached; null JSON if it's missing or invalid: logged once).
    static std::shared_ptr<const Json> get(const std::string& path);
    // Bumps whenever any cached asset's content changes (reload, set, revert).
    static uint64_t generation();

    // Replaces the cached content (the editor): users see it at once; the file
    // is only written by save().
    static void set(const std::string& path, Json content);
    static bool dirty(const std::string& path);   // set() since the last save / load
    static bool save(const std::string& path);    // writes the cached content
    static void revert(const std::string& path);  // back to the file's content
    // Writes `content` as a new file (or over one) and caches it.
    static bool create(const std::string& path, const Json& content);
    static bool exists(const std::string& path);   // the file exists on disk

    // Where save() writes: the source assets folder in a development build,
    // else the first asset search path.
    static std::filesystem::path writablePath(const std::string& path);

    // Engine: reloads cached files changed on disk (throttled; call every frame).
    static void poll(float dt);
    // Every cached path (editor lists).
    static std::vector<std::string> cachedPaths();
};

} // namespace ve
