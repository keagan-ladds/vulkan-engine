#pragma once

// =============================================================================
// TextureCook - turns an image file into a GPU-ready KTX2 file: block
// compressed, with its whole mip chain, so loading it is "read and upload"
// (no PNG decode, no mip generation, 4-8x less GPU memory).
//
//   Color  -> BC7 sRGB   (alpha kept)          mips averaged in linear light
//   Normal -> BC5        (X, Y; Z rebuilt by veUnpackNormal)   mips renormalized
//   Data   -> BC4        greyscale images (viewed as R,R,R,1)
//             BC7 linear anything else (packed ORM, masks with colour)
//   compress = false -> RGBA8 (still with mips)
//
// The cooked file remembers what it was made from (key "ve.cook": cooker
// version, the settings' cookHash, the source's size, time and content hash,
// and the settings themselves), so the AssetManager can tell a stale cooked
// file from a current one and a shipped build without sources still knows
// the sampler. HDR images aren't cooked (they load as float textures).
//
// Pure CPU work with no engine state: safe on job threads and in the vecook
// command-line tool.
// =============================================================================

#include "ve/assets/Ktx2.hpp"
#include "ve/assets/TextureSettings.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

namespace ve {

// Bump when the cooker's output changes: every cooked texture is redone.
constexpr uint32_t kTextureCookerVersion = 1;

// Images the cooker handles (by extension).
bool isCookableImage(const std::filesystem::path& file);
// Where the cooked copy of an asset lives: <cacheDir>/<asset path>.ktx2.
std::filesystem::path cookedTexturePath(const std::filesystem::path& cacheDir, const std::string& assetPath);

// 64-bit FNV-1a of a file's bytes (0 if unreadable).
uint64_t hashFileContent(const std::filesystem::path& file);

struct TextureCookResult {
    bool ok = false;
    std::string error;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0, levels = 0;
    uint64_t bytes = 0;     // of the cooked file's pixel data
    double milliseconds = 0;
};
// Cooks `source` into `output` (written via a temporary file, then renamed).
// `cancelled` (optional) is polled between block rows.
TextureCookResult cookTexture(const std::filesystem::path& source, const TextureSettings& settings,
                              const std::filesystem::path& output, const std::atomic<bool>* cancelled = nullptr);

enum class CookedState {
    Current,  // made by this cooker from these settings and the source as it is now (or no source to compare)
    Stale,    // the source, settings or cooker changed: cook again
    Unusable, // missing or unreadable
};
// Checks a cooked file (read with headerOnly is enough). `source` may not
// exist (shipped builds): then only the cooker version is checked, and
// `settingsOut` gets the settings stored in the file.
CookedState checkCookedTexture(const Ktx2Texture& cooked, const std::filesystem::path& source, const TextureSettings* settings,
                               TextureSettings* settingsOut = nullptr);

} // namespace ve
