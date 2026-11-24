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

#include "ve/assets/Cook.hpp"
#include "ve/assets/Ktx2.hpp"
#include "ve/assets/TextureSettings.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ve {

// Bump when the cooker's output changes: every cooked texture is redone.
constexpr uint32_t kTextureCookerVersion = 1;

// Images the cooker handles (by extension).
bool isCookableImage(const std::filesystem::path& file);
// Where the cooked copy of an asset lives: <cacheDir>/<asset path>.ktx2.
std::filesystem::path cookedTexturePath(const std::filesystem::path& cacheDir, const std::string& assetPath);

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

// Decodes one level of a cooked texture to 8-bit RGBA on the CPU (BC4 -> R,R,R,255;
// BC5 -> X, Y, rebuilt Z, 255, like veUnpackNormal; BC7 / RGBA8 as stored). For
// code that wants pixels while only the cooked copy exists (a packaged game).
bool decodeCookedTexture(const Ktx2Texture& texture, uint32_t level, uint32_t& width, uint32_t& height,
                         std::vector<uint8_t>& rgba);

// Checks a cooked file (read with headerOnly is enough). `source` may not
// exist (shipped builds): then only the cooker version is checked, and
// `settingsOut` gets the settings stored in the file.
CookedState checkCookedTexture(const Ktx2Texture& cooked, const std::filesystem::path& source, const TextureSettings* settings,
                               TextureSettings* settingsOut = nullptr);

} // namespace ve
