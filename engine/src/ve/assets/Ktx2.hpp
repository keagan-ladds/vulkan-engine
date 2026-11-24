#pragma once

// =============================================================================
// Ktx2 - reads and writes KTX 2.0 files (the Khronos GPU texture container):
// one 2D image with its mip levels, in any Vulkan format (the cooker writes
// BC4 / BC5 / BC7 and RGBA8), plus key/value metadata. No supercompression.
// Enough of the format for cooked textures; other tools (ktx info, texture
// viewers) can open the files.
// =============================================================================

#include "ve/gfx/Vk.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace ve {

struct Ktx2Texture {
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    // All levels in one block (level 0 = full size), level i at levelOffsets[i].
    std::vector<uint8_t> data;
    std::vector<VkDeviceSize> levelOffsets;
    std::vector<VkDeviceSize> levelSizes;
    std::vector<std::pair<std::string, std::string>> keyValues; // written sorted by key

    const std::string* value(const std::string& key) const;
};

// Bytes per 4x4 block for block-compressed formats, per texel otherwise (0 = unsupported).
uint32_t ktx2BlockBytes(VkFormat format);
bool ktx2IsBlockCompressed(VkFormat format);
// Byte size of one level of a width x height image.
VkDeviceSize ktx2LevelSize(VkFormat format, uint32_t width, uint32_t height);

bool writeKtx2(const std::filesystem::path& file, const Ktx2Texture& texture, std::string* error = nullptr);
// headerOnly: size, format and key/values, no pixel data (cheap validity checks).
bool readKtx2(const std::filesystem::path& file, Ktx2Texture& texture, bool headerOnly = false, std::string* error = nullptr);

} // namespace ve
