#pragma once

// =============================================================================
// Cookers - the registry of asset cookers: what vecook (cooking, checking,
// packaging) knows about each kind of cookable file, in one place. Adding a
// new kind (meshes, ...) = one registerCooker() call; vecook and
// `vecook --package` need no changes.
//
// Built in: "texture" (TextureCook.hpp: images -> KTX2) and "audio"
// (AudioCook.hpp: WAV / FLAC -> Ogg Vorbis). A game can register its own
// before calling into the tools (e.g. from a custom cooking tool's main()).
//
// The AssetManager has its own load paths per kind (it needs the cooked
// data in kind-specific ways); this registry is for the tools that treat
// every kind alike.
// =============================================================================

#include "ve/assets/Cook.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace ve {

/// One kind of cookable asset: which files, where their cooked copies go, how
/// to check and cook them (see the top of this file).
struct Cooker {
    /// What cook() did.
    struct Result {
        bool ok = false;      ///< The cooked file was written.
        std::string error;    ///< Why not (when !ok).
        std::string summary; ///< one line for logs: "1024x1024 BC7, 11 mips, 1.4 MB"
        uint64_t bytes = 0;  ///< of the cooked file
    };

    std::string name; ///< "texture"
    /// Settings files that sit next to a source ("`<image>`.texture"): read by
    /// the cooker, never shipped.
    std::vector<std::string> settingsExtensions;
    /// Does this cooker take `source`? (Extension, and its settings: e.g. a
    /// sound whose .audio says compress: false is shipped as it is.)
    std::function<bool(const std::filesystem::path& source)> handles;
    /// Where the cooked copy of the asset `assetPath` ("textures/a.png") goes.
    std::function<std::filesystem::path(const std::filesystem::path& cacheDir, const std::string& assetPath)> cookedPath;
    /// Is `cooked` current for `source` (with the source's own settings)?
    std::function<CookedState(const std::filesystem::path& cooked, const std::filesystem::path& source)> check;
    /// Cooks `source` into `cooked` (pure CPU work, run on job threads; poll `cancelled`).
    std::function<Result(const std::filesystem::path& source, const std::filesystem::path& cooked, const std::atomic<bool>* cancelled)> cook;
};

/// Every registered cooker (the built-in ones first).
const std::vector<Cooker>& cookers();
/// The cooker that takes `source`, or null (the file ships as it is).
const Cooker* cookerFor(const std::filesystem::path& source);
/// Is `file` a cooker's settings file (".texture", ".audio", ...)?
bool isCookerSettingsFile(const std::filesystem::path& file);
void registerCooker(Cooker cooker);

} // namespace ve
