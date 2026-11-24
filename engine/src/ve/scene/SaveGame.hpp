#pragma once

// =============================================================================
// Save games (F5): what changed in a level, captured as JSON and written to
// a slot.
//
//   ve::save::saveGame(scene, "quick", {{"wood", wood}});        // F5
//   if (auto game = ve::save::loadGame(scene, "quick")) wood = (*game)["wood"]; // F9
//
// A save holds, on top of the level file the game loads first:
//   - "moved":   world transforms of level objects that physics moves
//                (dynamic Rigidbodies: crates, thrown rocks);
//   - "removed": level objects that no longer exist (a collected pickup);
//   - "spawned": prefab instances created while playing (PrefabInstance on
//                a root that isn't a level object, e.g. player buildings):
//                prefab, id and transform; loading replaces the current set;
//   - "state":   whatever every ve::Saveable object or component writes
//                (saveState / loadState), keyed by the object's persistent
//                id, or its name for code-spawned objects (e.g. "Player"),
//                plus the component's type. The engine's Foliage (chopped /
//                picked instances) and Terrain (edits) are Saveable;
//   - "game":    free JSON from the game (counters, quest flags...).
// Loading applies it to the level as it is (the game reloads the level first
// if needed): removes, moves, recreates the spawned prefab instances (same
// ids, so their Saveables find their state), then loadState() on every
// Saveable it has a record for.
//
// Slots are files in save::folder(): EngineConfig::saveFolder, or the
// platform's per-user data folder ("<title>/saves"); VE_SAVE_DIR overrides.
// =============================================================================

#include "ve/scene/Reflection.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ve {

class Scene;

/// Implement on a GameObject or Component subclass to be part of save games.
class Saveable {
public:
    virtual ~Saveable() = default;
    /// Writes this object's state into `out` (a JSON object) when a game is saved.
    virtual void saveState(Json& out) const = 0;
    /// Restores the state saveState() wrote (`in` is that JSON; fields may be missing in older saves).
    virtual void loadState(const Json& in) = 0;
};

namespace save {

/// A save slot on disk (save::slots()).
struct SlotInfo {
    std::string name;  ///< The slot's name (what save::write / read take).
    std::filesystem::file_time_type time;  ///< When it was last written.
    uintmax_t bytes = 0;  ///< File size in bytes.
};

std::filesystem::path folder();
Json capture(Scene& scene);
void apply(Scene& scene, const Json& save);
bool write(const std::string& slot, const Json& save);
std::optional<Json> read(const std::string& slot);
std::vector<SlotInfo> slots(); ///< newest first
bool remove(const std::string& slot);

/// capture + the game's own data + write; read + apply, returning the game's data.
bool saveGame(Scene& scene, const std::string& slot, const Json& gameData = Json::object());
std::optional<Json> loadGame(Scene& scene, const std::string& slot);

/// Small per-user settings files ("settings", "controls"...), kept in
/// folder()/config so they never show up in slots(). Written atomically
/// (temporary file + rename); read returns nullopt if missing or damaged.
///   if (auto j = ve::save::readConfig("controls")) input.loadBindings(*j);
///   ve::save::writeConfig("controls", input.saveBindings());
std::optional<Json> readConfig(const std::string& name);
bool writeConfig(const std::string& name, const Json& json);

} // namespace save
} // namespace ve
