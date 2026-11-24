#include "ve/assets/Ktx2.hpp"

#include "ve/assets/Cook.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace ve {

namespace {

constexpr uint8_t kIdentifier[12] = {0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
constexpr size_t kHeaderSize = 12 + 9 * 4 + 4 * 4 + 2 * 8; // identifier, header, index = 80
constexpr size_t kLevelEntrySize = 3 * 8;

struct Writer {
    std::vector<uint8_t> bytes;
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) bytes.push_back(uint8_t(v >> (8 * i))); }
    void u64(uint64_t v) { for (int i = 0; i < 8; ++i) bytes.push_back(uint8_t(v >> (8 * i))); }
    void pad(size_t alignment) { while (bytes.size() % alignment) bytes.push_back(0); }
    void put32(size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) bytes[at + i] = uint8_t(v >> (8 * i)); }
    void put64(size_t at, uint64_t v) { for (int i = 0; i < 8; ++i) bytes[at + i] = uint8_t(v >> (8 * i)); }
};

uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t get64(const uint8_t* p) { return uint64_t(get32(p)) | uint64_t(get32(p + 4)) << 32; }

bool fail(std::string* error, std::string message)
{
    if (error) *error = std::move(message);
    return false;
}

// Khronos Data Format descriptor (required by KTX2): describes the texel
// layout for tools; our reader only uses vkFormat.
std::vector<uint32_t> dataFormatDescriptor(VkFormat format)
{
    struct Sample {
        uint32_t bitOffset, bitLength, channel;
        uint32_t upper;
    };
    uint32_t model = 1 /* RGBSDA */, transfer = 1 /* linear */, blockDim = 0, bytesPlane = 4;
    std::vector<Sample> samples;
    switch (format) {
    case VK_FORMAT_BC7_SRGB_BLOCK: transfer = 2; [[fallthrough]];
    case VK_FORMAT_BC7_UNORM_BLOCK: model = 134; blockDim = 0x0303; bytesPlane = 16; samples = {{0, 128, 0, 0xFFFFFFFFu}}; break;
    case VK_FORMAT_BC4_UNORM_BLOCK: model = 131; blockDim = 0x0303; bytesPlane = 8; samples = {{0, 64, 0, 0xFFFFFFFFu}}; break;
    case VK_FORMAT_BC5_UNORM_BLOCK: model = 132; blockDim = 0x0303; bytesPlane = 16; samples = {{0, 64, 0, 0xFFFFFFFFu}, {64, 64, 1, 0xFFFFFFFFu}}; break;
    case VK_FORMAT_R8G8B8A8_SRGB: transfer = 2; [[fallthrough]];
    default: samples = {{0, 8, 0, 255}, {8, 8, 1, 255}, {16, 8, 2, 255}, {24, 8, 15, 255}}; break;
    }
    std::vector<uint32_t> d;
    const uint32_t blockSize = 24 + 16 * uint32_t(samples.size());
    d.push_back(4 + blockSize);                          // dfdTotalSize
    d.push_back(0);                                      // vendorId KHR, descriptorType basic
    d.push_back(2u | blockSize << 16);                   // versionNumber 2, descriptorBlockSize
    d.push_back(model | 1u << 8 | transfer << 16);       // colour model, BT.709 primaries, transfer, flags 0
    d.push_back(blockDim);                               // texel block dimensions - 1
    d.push_back(bytesPlane);                             // bytesPlane0
    d.push_back(0);
    for (const Sample& s : samples) {
        // The alpha of an sRGB format is linear (qualifier 0x10 on the channel).
        const uint32_t channel = s.channel | (transfer == 2 && s.channel == 15 ? 0x10u : 0u);
        d.push_back(s.bitOffset | (s.bitLength - 1) << 16 | channel << 24);
        d.push_back(0);       // sample position
        d.push_back(0);       // lower
        d.push_back(s.upper); // upper
    }
    return d;
}

} // namespace

const std::string* Ktx2Texture::value(const std::string& key) const
{
    for (const auto& [k, v] : keyValues)
        if (k == key) return &v;
    return nullptr;
}

uint32_t ktx2BlockBytes(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_BC4_UNORM_BLOCK: return 8;
    case VK_FORMAT_BC5_UNORM_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK:
    case VK_FORMAT_BC7_SRGB_BLOCK: return 16;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB: return 4;
    default: return 0;
    }
}

bool ktx2IsBlockCompressed(VkFormat format)
{
    return format == VK_FORMAT_BC4_UNORM_BLOCK || format == VK_FORMAT_BC5_UNORM_BLOCK ||
           format == VK_FORMAT_BC7_UNORM_BLOCK || format == VK_FORMAT_BC7_SRGB_BLOCK;
}

VkDeviceSize ktx2LevelSize(VkFormat format, uint32_t width, uint32_t height)
{
    if (ktx2IsBlockCompressed(format)) return VkDeviceSize((width + 3) / 4) * ((height + 3) / 4) * ktx2BlockBytes(format);
    return VkDeviceSize(width) * height * ktx2BlockBytes(format);
}

bool writeKtx2(const std::filesystem::path& file, const Ktx2Texture& t, std::string* error)
{
    const uint32_t levels = static_cast<uint32_t>(t.levelOffsets.size());
    if (levels == 0 || t.levelSizes.size() != levels || ktx2BlockBytes(t.format) == 0) return fail(error, "nothing to write");
    Writer w;
    w.bytes.insert(w.bytes.end(), kIdentifier, kIdentifier + 12);
    w.u32(uint32_t(t.format));
    w.u32(1);          // typeSize
    w.u32(t.width);
    w.u32(t.height);
    w.u32(0);          // pixelDepth (2D)
    w.u32(0);          // layerCount (not an array)
    w.u32(1);          // faceCount
    w.u32(levels);
    w.u32(0);          // supercompressionScheme: none
    const size_t index = w.bytes.size();
    w.bytes.resize(w.bytes.size() + 4 * 4 + 2 * 8); // filled in below
    const size_t levelIndex = w.bytes.size();
    w.bytes.resize(w.bytes.size() + levels * kLevelEntrySize);

    // Data format descriptor.
    const size_t dfdOffset = w.bytes.size();
    for (uint32_t v : dataFormatDescriptor(t.format)) w.u32(v);
    const size_t dfdLength = w.bytes.size() - dfdOffset;

    // Key/value data, sorted by key.
    auto kv = t.keyValues;
    std::sort(kv.begin(), kv.end());
    const size_t kvdOffset = w.bytes.size();
    for (const auto& [key, value] : kv) {
        w.u32(uint32_t(key.size() + 1 + value.size() + 1));
        w.bytes.insert(w.bytes.end(), key.begin(), key.end());
        w.bytes.push_back(0);
        w.bytes.insert(w.bytes.end(), value.begin(), value.end());
        w.bytes.push_back(0);
        w.pad(4);
    }
    const size_t kvdLength = w.bytes.size() - kvdOffset;

    // Levels, smallest first, each aligned to lcm(block size, 4).
    const size_t align = std::max<size_t>(4, ktx2BlockBytes(t.format)); // (4, 8 and 16: the lcm is the larger)
    for (uint32_t i = levels; i-- > 0;) {
        w.pad(align);
        const size_t at = w.bytes.size();
        const uint8_t* src = t.data.data() + t.levelOffsets[i];
        w.bytes.insert(w.bytes.end(), src, src + t.levelSizes[i]);
        w.put64(levelIndex + i * kLevelEntrySize + 0, at);
        w.put64(levelIndex + i * kLevelEntrySize + 8, t.levelSizes[i]);
        w.put64(levelIndex + i * kLevelEntrySize + 16, t.levelSizes[i]);
    }
    w.put32(index + 0, uint32_t(dfdOffset));
    w.put32(index + 4, uint32_t(dfdLength));
    w.put32(index + 8, kvdLength ? uint32_t(kvdOffset) : 0);
    w.put32(index + 12, uint32_t(kvdLength));
    w.put64(index + 16, 0); // no supercompression global data
    w.put64(index + 24, 0);

    // (Via a temporary file + rename: a reader, or a crash, never sees half a file.)
    return writeFileReplacing(file, w.bytes.data(), w.bytes.size(), error);
}

bool readKtx2(const std::filesystem::path& file, Ktx2Texture& t, bool headerOnly, std::string* error)
{
    std::ifstream in(file, std::ios::binary);
    if (!in) return fail(error, "can't open " + file.string());
    uint8_t head[kHeaderSize];
    if (!in.read(reinterpret_cast<char*>(head), kHeaderSize) || std::memcmp(head, kIdentifier, 12) != 0)
        return fail(error, file.string() + " is not a KTX2 file");
    t = {};
    t.format = VkFormat(get32(head + 12));
    t.width = get32(head + 20);
    t.height = get32(head + 24);
    const uint32_t depth = get32(head + 28), layers = get32(head + 32), faces = get32(head + 36);
    const uint32_t levels = std::max(1u, get32(head + 40));
    if (get32(head + 44) != 0) return fail(error, file.string() + ": supercompression isn't supported");
    if (depth > 1 || layers > 1 || faces != 1 || t.width == 0 || t.height == 0 || levels > 16 || ktx2BlockBytes(t.format) == 0)
        return fail(error, file.string() + ": not a plain 2D texture in a supported format");
    const uint32_t kvdOffset = get32(head + 56), kvdLength = get32(head + 60);

    std::vector<uint8_t> levelIndex(levels * kLevelEntrySize);
    if (!in.read(reinterpret_cast<char*>(levelIndex.data()), std::streamsize(levelIndex.size())))
        return fail(error, file.string() + ": truncated");
    if (kvdLength > 0 && kvdLength < (1u << 20)) {
        std::vector<uint8_t> kvd(kvdLength);
        in.seekg(kvdOffset);
        if (!in.read(reinterpret_cast<char*>(kvd.data()), kvdLength)) return fail(error, file.string() + ": truncated");
        for (size_t p = 0; p + 4 <= kvd.size();) {
            const uint32_t len = get32(kvd.data() + p);
            if (len == 0 || p + 4 + len > kvd.size()) break;
            const char* s = reinterpret_cast<const char*>(kvd.data() + p + 4);
            const size_t keyLen = size_t(std::find(s, s + len, '\0') - s);
            std::string key(s, keyLen), value;
            if (keyLen + 1 < len) {
                value.assign(s + keyLen + 1, len - keyLen - 1);
                if (!value.empty() && value.back() == '\0') value.pop_back();
            }
            t.keyValues.emplace_back(std::move(key), std::move(value));
            p += 4 + ((len + 3) & ~3u);
        }
    }
    if (headerOnly) return true;

    // Levels into one block, largest first.
    VkDeviceSize total = 0;
    for (uint32_t i = 0; i < levels; ++i) total += get64(levelIndex.data() + i * kLevelEntrySize + 8);
    t.data.resize(size_t(total));
    VkDeviceSize at = 0;
    for (uint32_t i = 0; i < levels; ++i) {
        const uint64_t offset = get64(levelIndex.data() + i * kLevelEntrySize);
        const uint64_t length = get64(levelIndex.data() + i * kLevelEntrySize + 8);
        if (length != ktx2LevelSize(t.format, std::max(1u, t.width >> i), std::max(1u, t.height >> i)))
            return fail(error, file.string() + ": level " + std::to_string(i) + " has the wrong size");
        in.seekg(std::streamoff(offset));
        if (!in.read(reinterpret_cast<char*>(t.data.data() + at), std::streamsize(length))) return fail(error, file.string() + ": truncated");
        t.levelOffsets.push_back(at);
        t.levelSizes.push_back(length);
        at += length;
    }
    return true;
}

} // namespace ve
