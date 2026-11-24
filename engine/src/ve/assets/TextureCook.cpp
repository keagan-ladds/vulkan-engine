#include "ve/assets/TextureCook.hpp"

#include <bc7decomp.h>
#include <bc7enc.h>
#include <rgbcx.h>
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <vector>

namespace ve {

namespace {

// RGBA8 pixels of one image level.
struct Level {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> px;
    const uint8_t* at(uint32_t x, uint32_t y) const { return &px[(size_t(std::min(y, h - 1)) * w + std::min(x, w - 1)) * 4]; }
};

const std::array<float, 256>& srgbToLinearTable()
{
    static const std::array<float, 256> t = [] {
        std::array<float, 256> a{};
        for (int i = 0; i < 256; ++i) {
            const float c = float(i) / 255.0f;
            a[size_t(i)] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        return a;
    }();
    return t;
}

uint8_t linearToSrgb8(float v)
{
    v = std::clamp(v, 0.0f, 1.0f);
    const float s = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
    return uint8_t(std::lround(s * 255.0f));
}

uint8_t unorm8(float v) { return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }

// Half size (2x2 box; the last row / column repeats for odd sizes). Colour
// is averaged in linear light (averaging sRGB values darkens mips), normals
// are renormalized (averaging shortens them, which flattens the lighting).
Level downsample(const Level& src, TextureType type)
{
    Level dst;
    dst.w = std::max(1u, src.w / 2);
    dst.h = std::max(1u, src.h / 2);
    dst.px.resize(size_t(dst.w) * dst.h * 4);
    const auto& lin = srgbToLinearTable();
    for (uint32_t y = 0; y < dst.h; ++y) {
        for (uint32_t x = 0; x < dst.w; ++x) {
            const uint8_t* p[4] = {src.at(2 * x, 2 * y), src.at(2 * x + 1, 2 * y), src.at(2 * x, 2 * y + 1), src.at(2 * x + 1, 2 * y + 1)};
            uint8_t* out = &dst.px[(size_t(y) * dst.w + x) * 4];
            float c[4] = {0, 0, 0, 0};
            for (const uint8_t* s : p) {
                for (int k = 0; k < 4; ++k) {
                    const float v = type == TextureType::Color && k < 3 ? lin[s[k]] : float(s[k]) / 255.0f;
                    c[k] += (type == TextureType::Normal && k < 3 ? v * 2.0f - 1.0f : v) * 0.25f;
                }
            }
            if (type == TextureType::Normal) {
                const float len = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
                for (int k = 0; k < 3; ++k) out[k] = unorm8((len > 1e-6f ? c[k] / len : (k == 2 ? 1.0f : 0.0f)) * 0.5f + 0.5f);
            } else {
                for (int k = 0; k < 3; ++k) out[k] = type == TextureType::Color ? linearToSrgb8(c[k]) : unorm8(c[k]);
            }
            out[3] = unorm8(c[3]);
        }
    }
    return dst;
}

void initEncoders()
{
    static std::once_flag once;
    std::call_once(once, [] {
        bc7enc_compress_block_init();
        rgbcx::init();
    });
}

// Encodes one level into `out` (appended). False if cancelled.
bool encodeLevel(const Level& l, VkFormat format, const bc7enc_compress_block_params& bc7, std::vector<uint8_t>& out,
                 const std::atomic<bool>* cancelled)
{
    if (!ktx2IsBlockCompressed(format)) {
        out.insert(out.end(), l.px.begin(), l.px.end());
        return true;
    }
    const uint32_t bx = (l.w + 3) / 4, by = (l.h + 3) / 4, blockBytes = ktx2BlockBytes(format);
    const size_t start = out.size();
    out.resize(start + size_t(bx) * by * blockBytes);
    uint8_t block[16 * 4];
    for (uint32_t y = 0; y < by; ++y) {
        if (cancelled && cancelled->load(std::memory_order_relaxed)) return false;
        for (uint32_t x = 0; x < bx; ++x) {
            // 4x4 texels (edges repeat for sizes that aren't multiples of 4).
            for (uint32_t j = 0; j < 4; ++j)
                for (uint32_t i = 0; i < 4; ++i) std::memcpy(&block[(j * 4 + i) * 4], l.at(x * 4 + i, y * 4 + j), 4);
            uint8_t* dst = &out[start + (size_t(y) * bx + x) * blockBytes];
            switch (format) {
            case VK_FORMAT_BC4_UNORM_BLOCK: rgbcx::encode_bc4_hq(dst, block, 4); break;
            case VK_FORMAT_BC5_UNORM_BLOCK: rgbcx::encode_bc5_hq(dst, block, 0, 1, 4); break;
            default: bc7enc_compress_block(dst, block, &bc7); break;
            }
        }
    }
    return true;
}

} // namespace

bool isCookableImage(const std::filesystem::path& file)
{
    std::string ext = file.extension().string();
    for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" || ext == ".psd" || ext == ".gif";
}

std::filesystem::path cookedTexturePath(const std::filesystem::path& cacheDir, const std::string& assetPath)
{
    return cacheDir / std::filesystem::path(assetPath + ".ktx2");
}

TextureCookResult cookTexture(const std::filesystem::path& source, const TextureSettings& settings,
                              const std::filesystem::path& output, const std::atomic<bool>* cancelled)
{
    const auto t0 = std::chrono::steady_clock::now();
    TextureCookResult r;
    if (!isCookableImage(source)) {
        r.error = "not a cookable image";
        return r;
    }
    // Stamp the source before reading it: if it changes meanwhile, the
    // cooked file looks stale next time rather than wrongly current.
    const nlohmann::json stamp = makeCookStamp(source, kTextureCookerVersion, settings.cookHash(), settings.toJson());

    int w = 0, h = 0, channels = 0;
    stbi_uc* pixels = stbi_load(source.string().c_str(), &w, &h, &channels, 4);
    if (!pixels) {
        r.error = std::string("can't decode: ") + stbi_failure_reason();
        return r;
    }
    Level level;
    level.w = uint32_t(w);
    level.h = uint32_t(h);
    level.px.assign(pixels, pixels + size_t(w) * h * 4);
    stbi_image_free(pixels);

    // Format.
    VkFormat format = VK_FORMAT_BC7_SRGB_BLOCK;
    if (!settings.compress) {
        format = settings.srgb() ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    } else if (settings.type == TextureType::Normal) {
        format = VK_FORMAT_BC5_UNORM_BLOCK;
    } else if (settings.type == TextureType::Data) {
        bool grey = channels <= 2;
        if (!grey) {
            grey = true;
            for (size_t i = 0; i < level.px.size() && grey; i += 4)
                grey = level.px[i] == level.px[i + 1] && level.px[i] == level.px[i + 2] && level.px[i + 3] == 255;
        }
        format = grey ? VK_FORMAT_BC4_UNORM_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
    }

    while (settings.maxSize > 0 && std::max(level.w, level.h) > settings.maxSize && std::max(level.w, level.h) > 1)
        level = downsample(level, settings.type);

    initEncoders();
    bc7enc_compress_block_params bc7;
    bc7enc_compress_block_params_init(&bc7);
    if (!settings.srgb()) bc7enc_compress_block_params_init_linear_weights(&bc7); // data: every channel counts the same

    Ktx2Texture out;
    out.format = format;
    out.width = level.w;
    out.height = level.h;
    const uint32_t levels = settings.mipmaps ? uint32_t(std::bit_width(std::max(level.w, level.h))) : 1u;
    for (uint32_t i = 0; i < levels; ++i) {
        if (i > 0) level = downsample(level, settings.type);
        const size_t before = out.data.size();
        if (!encodeLevel(level, format, bc7, out.data, cancelled)) {
            r.error = "cancelled";
            return r;
        }
        out.levelOffsets.push_back(before);
        out.levelSizes.push_back(out.data.size() - before);
    }
    out.keyValues.emplace_back("KTXwriter", "vulkan-engine texture cooker " + std::to_string(kTextureCookerVersion));
    out.keyValues.emplace_back("ve.cook", stamp.dump());
    if (!writeKtx2(output, out, &r.error)) return r;

    r.ok = true;
    r.format = format;
    r.width = out.width;
    r.height = out.height;
    r.levels = levels;
    r.bytes = out.data.size();
    r.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

bool decodeCookedTexture(const Ktx2Texture& t, uint32_t level, uint32_t& width, uint32_t& height, std::vector<uint8_t>& rgba)
{
    if (level >= t.levelOffsets.size()) return false;
    width = std::max(1u, t.width >> level);
    height = std::max(1u, t.height >> level);
    rgba.assign(size_t(width) * height * 4, 255);
    const uint8_t* data = t.data.data() + t.levelOffsets[level];
    if (!ktx2IsBlockCompressed(t.format)) {
        if (t.levelSizes[level] < rgba.size()) return false;
        std::memcpy(rgba.data(), data, rgba.size());
        return true;
    }
    initEncoders(); // (rgbcx's tables)
    const uint32_t bx = (width + 3) / 4, by = (height + 3) / 4, blockBytes = ktx2BlockBytes(t.format);
    if (t.levelSizes[level] < size_t(bx) * by * blockBytes) return false;
    uint8_t block[16 * 4];
    for (uint32_t y = 0; y < by; ++y) {
        for (uint32_t x = 0; x < bx; ++x) {
            const uint8_t* src = data + (size_t(y) * bx + x) * blockBytes;
            std::memset(block, 255, sizeof(block));
            switch (t.format) {
            case VK_FORMAT_BC4_UNORM_BLOCK:
                rgbcx::unpack_bc4(src, block, 4);
                for (int i = 0; i < 16; ++i) block[i * 4 + 1] = block[i * 4 + 2] = block[i * 4];
                break;
            case VK_FORMAT_BC5_UNORM_BLOCK:
                rgbcx::unpack_bc5(src, block, 0, 1, 4);
                for (int i = 0; i < 16; ++i) { // Z from X / Y (unit normals)
                    const float nx = block[i * 4] / 127.5f - 1.0f, ny = block[i * 4 + 1] / 127.5f - 1.0f;
                    block[i * 4 + 2] = unorm8(std::sqrt(std::max(0.0f, 1.0f - nx * nx - ny * ny)) * 0.5f + 0.5f);
                }
                break;
            default:
                bc7decomp::unpack_bc7(src, reinterpret_cast<bc7decomp::color_rgba*>(block));
                break;
            }
            for (uint32_t j = 0; j < 4 && y * 4 + j < height; ++j)
                for (uint32_t i = 0; i < 4 && x * 4 + i < width; ++i)
                    std::memcpy(&rgba[((size_t(y) * 4 + j) * width + x * 4 + i) * 4], &block[(j * 4 + i) * 4], 4);
        }
    }
    return true;
}

CookedState checkCookedTexture(const Ktx2Texture& cooked, const std::filesystem::path& source, const TextureSettings* settings,
                               TextureSettings* settingsOut)
{
    const std::string* metaText = cooked.value("ve.cook");
    if (!metaText) return CookedState::Unusable;
    const nlohmann::json stamp = nlohmann::json::parse(*metaText, nullptr, false);
    if (settingsOut && stamp.is_object()) settingsOut->applyJson(stamp.value("settings", nlohmann::json::object()));
    const uint64_t hash = settings ? settings->cookHash() : 0;
    return checkCookStamp(stamp, source, kTextureCookerVersion, settings ? &hash : nullptr);
}

} // namespace ve
