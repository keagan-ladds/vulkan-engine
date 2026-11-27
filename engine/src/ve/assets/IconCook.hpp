#pragma once

// =============================================================================
// IconCook - menu icons of prefabs (build menus, inventories, shops), cooked
// to `<cache>/<prefab path>.veicon`: the rendered picture (RGBA8) and a stamp
// (cooker version, the prefab's "icon" settings, the prefab file and the
// files it uses: models, textures, materials) so a stale icon is re-rendered.
//
// A prefab opts in with an "icon" section next to its "object":
//   { "version": 1, "object": {...},
//     "icon": {"size": 192, "yaw": 35, "pitch": 22, "padding": 0.06, "key": "kit-2"} }
// ("key" is any text that, when changed, re-renders the icon: bump it when
// code that builds the prefab's meshes changes.) `vecook` cooks those icons
// on its headless renderer (the prefab still ships: it's also loaded as a
// prefab). ui::prefabIcon (ui/PrefabIcon.hpp) loads them, and renders and
// caches any icon that's missing or stale (also for prefabs without "icon").
// Rendering needs the renderer, so the ui module registers the baker
// (setIconBaker), like the model cooker's LOD step.
// =============================================================================

#include "ve/assets/Cook.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace ve {

/// Bump when the icon file format or how icons are rendered changes: every icon re-renders.
constexpr uint32_t kIconCookerVersion = 1;

/// A prefab's "icon" settings (an empty object if it has none or can't be read).
nlohmann::json prefabIconSettings(const std::filesystem::path& prefabFile);
/// Whether `file` is a prefab with an "icon" section (what the icon cooker takes).
bool isIconPrefab(const std::filesystem::path& file);
/// Where the cooked icon of the prefab asset `assetPath` lives: <cacheDir>/<assetPath>.veicon.
std::filesystem::path cookedIconPath(const std::filesystem::path& cacheDir, const std::string& assetPath);

/// Why this program can't render `prefab`'s icon ("" = it can): its objects or
/// components are of types not registered here (game types, when run from
/// vecook rather than the game's own `--cook`).
std::string iconUnsupportedHere(const std::filesystem::path& prefab);

/// The stamp of an icon rendered now from `prefab` with `settings`, made from
/// `dependencies` too (the files the prefab uses).
nlohmann::json makeIconStamp(const std::filesystem::path& prefab, const nlohmann::json& settings,
                             const std::vector<std::filesystem::path>& dependencies);
/// Checks a cooked icon against its prefab, settings and dependencies. A
/// missing prefab file (packaged game) only checks the cooker version.
CookedState checkCookedIcon(const std::filesystem::path& cooked, const std::filesystem::path& prefab, const nlohmann::json& settings);
/// Writes a cooked icon (size x size RGBA8, rows top first). False (and `error`) on failure.
bool writeCookedIcon(const std::filesystem::path& file, const nlohmann::json& stamp, uint32_t size, const std::vector<uint8_t>& rgba,
                     std::string* error = nullptr);
/// Reads a cooked icon: its stamp and (when `rgba` isn't null) the picture.
/// False if it's missing or unreadable.
bool readCookedIcon(const std::filesystem::path& file, nlohmann::json* stamp, uint32_t* size, std::vector<uint8_t>* rgba);

/// Renders a prefab's icon: fills `size`, `rgba` (size x size RGBA8) and the
/// files it depends on. Main thread, needs a running Engine. Registered by the
/// ui module. @return false (and `error`) on failure.
using IconBaker = std::function<bool(const std::filesystem::path& prefab, const nlohmann::json& settings, uint32_t& size,
                                     std::vector<uint8_t>& rgba, std::vector<std::filesystem::path>& dependencies, std::string& error)>;
/// Registers the icon renderer (the ui module, at startup).
void setIconBaker(IconBaker baker);
/// The registered icon renderer (null before the ui module registers it).
const IconBaker& iconBaker();

/// What cookIcon did.
struct IconCookResult {
    bool ok = false;          ///< The cooked file was written.
    std::string error;        ///< Why not (when !ok).
    uint32_t size = 0;        ///< Icon width = height (pixels).
    uint64_t bytes = 0;       ///< Size of the cooked file.
};
/// Renders `prefab`'s icon (its "icon" settings) into `output`. Needs a
/// running Engine and the main thread.
IconCookResult cookIcon(const std::filesystem::path& prefab, const std::filesystem::path& output);

} // namespace ve
