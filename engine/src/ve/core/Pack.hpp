#pragma once

// =============================================================================
// Pack - .vepak archives: many files in one, each compressed on its own
// (zstd), so any entry can be read without unpacking the rest.
//
// Written by `vecook --package` (PackWriter); read by the virtual file system
// (FileSystem.hpp), which mounts <exe dir>/*.vepak at startup so their
// entries look like files under the executable's folder. Entry names are
// paths relative to that folder with '/' separators ("assets/scenes/a.json",
// "cooked/textures/rock.png.ktx2", "shaders/standard.frag.spv").
//
// Layout (little endian):
//   header   8 bytes "VEPAK\0\0\1" (magic + format version 1)
//            u64 index offset, u64 index size (stored), u64 index size (raw),
//            u32 entry count, u32 flags (0)
//   data     the entries, one after another, each 16-byte aligned
//   index    zstd-compressed list, per entry: u32 name length, name (UTF-8),
//            u64 offset, u64 stored size, u64 size, u8 method (0 stored,
//            1 zstd), u64 content hash (FNV-1a of the uncompressed bytes)
// =============================================================================

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ve {

/// Reads a .vepak archive (written by PackWriter / vecook --package): an index
/// of named entries, each zstd-compressed or stored. Usually used through ve::vfs.
class PackReader {
public:
    /// One file in the archive.
    struct Entry {
        std::string name;  ///< Path inside the archive, '/' separated ("assets/textures/a.png").
        uint64_t offset = 0;  ///< Byte offset of the stored data in the archive file.
        uint64_t storedSize = 0;  ///< Bytes stored in the archive (compressed size).
        uint64_t size = 0;  ///< Bytes once uncompressed.
        uint8_t method = 0;  ///< 0 = stored as is, 1 = zstd.
        uint64_t hash = 0;  ///< FNV-1a hash of the uncompressed bytes (integrity check).
    };

    /// Reads the index. False (and `error`) if it isn't a readable .vepak.
    bool open(const std::filesystem::path& file, std::string* error = nullptr);
    /// The archive file opened.
    const std::filesystem::path& file() const { return m_file; }

    /// The entry for a name ('/' separated); exact match first, then ignoring
    /// case (so a path that works on a case-insensitive disk works here too).
    const Entry* find(const std::string& name) const;
    /// The entry's uncompressed bytes. Thread-safe.
    std::optional<std::vector<uint8_t>> read(const Entry& entry) const;
    /// Every entry, in archive order.
    const std::vector<Entry>& entries() const { return m_entries; }

private:
    std::filesystem::path m_file;
    std::vector<Entry> m_entries;
    std::unordered_map<std::string, size_t> m_byName;
    std::unordered_map<std::string, size_t> m_byLowerName;
    mutable std::mutex m_lock; ///< m_stream is shared by reading threads
    mutable std::ifstream m_stream;
};

/// Writes a .vepak archive: begin(), add() each file, finish().
class PackWriter {
public:
    /// `level`: zstd level (1 fast .. 19 small; 0 = store everything).
    bool begin(const std::filesystem::path& file, int level = 9, std::string* error = nullptr);
    /// Adds an entry; compressed unless that saves under 3% (already
    /// compressed data: JPEG, Ogg, ...), then stored as is.
    bool add(const std::string& name, const void* data, size_t size);
    /// Writes the index and closes the file. False (and `error`) on a write failure.
    bool finish(std::string* error = nullptr);

    /// Total uncompressed bytes added so far.
    uint64_t rawBytes() const { return m_raw; }
    /// Total bytes written for the entries (after compression).
    uint64_t storedBytes() const { return m_stored; }
    /// Number of entries added.
    size_t count() const { return m_entries.size(); }

private:
    std::ofstream m_out;
    std::filesystem::path m_file;
    int m_level = 9;
    std::vector<PackReader::Entry> m_entries;
    uint64_t m_raw = 0, m_stored = 0;
};

/// 64-bit FNV-1a.
uint64_t fnv1a64(const void* data, size_t size, uint64_t seed = 1469598103934665603ull);

} // namespace ve
