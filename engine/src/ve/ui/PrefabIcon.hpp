#pragma once

// =============================================================================
// PrefabIcon - a menu icon for a prefab (build menus, inventories, shops):
//
//   image.texture = ve::ui::prefabIcon("prefabs/building/wall_mud.prefab");
//
// Where the picture comes from, first that exists:
//   1. a hand-made image next to the prefab: `<prefab>.png` (staged by an artist);
//   2. the cooked icon (`<cache>/<prefab>.veicon`, assets/IconCook.hpp), if current;
//   3. rendered now: the prefab is instantiated into a private scene (never
//      drawn, nothing else sees it), its meshes are drawn with their materials
//      (renderIconPixels), and the result is written to the cook cache for the
//      next run (when cooking on demand). vecook renders them ahead of time.
//
// The view comes from the prefab's "icon" section (all optional):
//   "icon": {"size": 192, "yaw": 35, "pitch": 22, "padding": 0.06,
//            "sun": [x, y, z], "key": "anything: change it to re-render"}
// Prefabs without one get the defaults. Icons are cached per path (one
// texture per prefab for the whole run).
// =============================================================================

#include "ve/ui/IconBaker.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ve::ui {

/// A prefab's icon (see the top of this file); null if the prefab can't be loaded or draws nothing.
std::shared_ptr<Texture> prefabIcon(const std::string& prefabPath);

/// The view and size an "icon" section asks for.
/// @param size receives the icon's width = height in pixels (default 192).
IconView iconViewFromJson(const nlohmann::json& settings, uint32_t& size);

/// Renders a prefab's icon (the "icon" settings `settings`): size x size RGBA8
/// pixels, plus the files the prefab draws with (the cook stamp's
/// dependencies). Main thread. False (and `error`) if nothing could be drawn.
bool renderPrefabIcon(const std::filesystem::path& prefab, const nlohmann::json& settings, uint32_t& size, std::vector<uint8_t>& rgba,
                      std::vector<std::filesystem::path>& dependencies, std::string& error);

/// Where this run's prefab icons came from (tests, the F1 overlay).
struct PrefabIconStats {
    uint32_t handMade = 0; ///< `<prefab>.png` images
    uint32_t cooked = 0;   ///< current cooked icons
    uint32_t rendered = 0; ///< rendered at load (missing or stale)
};
/// This run's counters.
const PrefabIconStats& prefabIconStats();
/// Forgets the cached icon textures (they're loaded again on the next prefabIcon call).
void clearPrefabIcons();

/// Registers renderPrefabIcon as the icon cooker's renderer (assets/IconCook.hpp; the Engine does at startup).
void registerPrefabIconBaker();

} // namespace ve::ui
