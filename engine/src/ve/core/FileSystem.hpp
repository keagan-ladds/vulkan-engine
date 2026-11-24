#pragma once

// =============================================================================
// FileSystem (ve::vfs) - how the engine reads game data: from disk, or from
// mounted .vepak archives (Pack.hpp) when the disk doesn't have the file.
//
// An archive overlays a folder: mounted at <exe dir>, its entry
// "assets/scenes/a.json" is the file <exe dir>/assets/scenes/a.json. So code
// keeps building ordinary paths and only reads through here:
//
//   if (auto bytes = ve::vfs::read(path)) ...            // whole file
//   if (auto in = ve::vfs::open(path)) json = Json::parse(*in);  // seekable stream
//   ve::vfs::exists(path), ve::vfs::list(folder, true)
//
// The Engine mounts <exe dir>/*.vepak at startup (alphabetically; a later
// archive wins over an earlier one, the disk wins over both), so a packaged
// game (vecook --package) needs nothing else. Anything that reads game data
// - a new file type included - should go through here to work packaged.
// User data (saves, settings, caches) is plain disk I/O, not this.
//
// Thread-safe for reads; mount / unmount on the main thread at startup.
// =============================================================================

#include <cstdint>
#include <filesystem>
#include <istream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ve::vfs {

/// Mounts an archive over the folder `root`. False (and `error`) if it can't be read.
bool mount(const std::filesystem::path& archive, const std::filesystem::path& root, std::string* error = nullptr);
/// Mounts every *.vepak in `folder` over that folder; returns how many.
int mountAll(const std::filesystem::path& folder);
void unmountAll();
/// The mounted archives, in lookup order (last mounted first).
std::vector<std::filesystem::path> mounted();

/// A file (not a folder) on disk or in an archive.
bool exists(const std::filesystem::path& file);
/// True if `file` isn't on disk but an archive has it.
bool inArchive(const std::filesystem::path& file);
std::optional<uint64_t> fileSize(const std::filesystem::path& file);

/// The whole file, or nothing if it doesn't exist / can't be read.
std::optional<std::vector<uint8_t>> read(const std::filesystem::path& file);
std::optional<std::string> readText(const std::filesystem::path& file);
/// A seekable binary stream (disk: the file; archive: its bytes in memory), or null.
std::unique_ptr<std::istream> open(const std::filesystem::path& file);

/// Files under `folder` (disk and archives merged, sorted, no duplicates).
std::vector<std::filesystem::path> list(const std::filesystem::path& folder, bool recursive);

} // namespace ve::vfs
