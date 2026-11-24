#pragma once

// =============================================================================
// Cook - what every asset cooker shares (TextureCook, AudioCook): the stamp a
// cooked file carries about its source, and the check whether it's current.
//
// A stamp is JSON: the cooker's version, the hash of the settings that shape
// the output, the source's size, modification time and content hash, and the
// settings themselves (so a shipped build without sources still knows them).
// Current = same cooker version and settings, and the source has the same
// size and either the same time or (a checkout, a copy) the same content.
// =============================================================================

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

namespace ve {

enum class CookedState {
    Current,  ///< made by this cooker from these settings and the source as it is now (or no source to compare)
    Stale,    ///< the source, settings or cooker changed: cook again
    Unusable, ///< missing or unreadable
};

/// 64-bit FNV-1a of a file's bytes (0 if unreadable).
uint64_t hashFileContent(const std::filesystem::path& file);
std::string cookHex(uint64_t value);

/// The stamp for a cooked file made now from `source`. Take it BEFORE reading
/// the source: if the file changes meanwhile, the result looks stale next time.
nlohmann::json makeCookStamp(const std::filesystem::path& source, uint32_t cookerVersion, uint64_t settingsHash,
                             const nlohmann::json& settings);
/// Checks a stamp. A missing source (shipped build) only checks the version.
/// `settingsHash` null: don't compare settings (no source to read them from).
CookedState checkCookStamp(const nlohmann::json& stamp, const std::filesystem::path& source, uint32_t cookerVersion,
                           const uint64_t* settingsHash);

/// Writes `file` via "<file>.tmp" + rename, so a reader (or a crash) never sees
/// half a file; creates the folder. False (and `error`) on failure.
bool writeFileReplacing(const std::filesystem::path& file, const void* data, size_t size, std::string* error = nullptr);

} // namespace ve
