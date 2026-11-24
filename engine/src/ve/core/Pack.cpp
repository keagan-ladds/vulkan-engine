#include "ve/core/Pack.hpp"

#include <zstd.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace ve {

namespace {

constexpr char kMagic[8] = {'V', 'E', 'P', 'A', 'K', 0, 0, 1};
constexpr size_t kHeaderSize = 8 + 8 + 8 + 8 + 4 + 4;

template <class T>
void put(std::vector<uint8_t>& b, T v)
{
    for (size_t i = 0; i < sizeof(T); ++i) b.push_back(uint8_t(uint64_t(v) >> (8 * i)));
}
template <class T>
T get(const uint8_t* p)
{
    uint64_t v = 0;
    for (size_t i = 0; i < sizeof(T); ++i) v |= uint64_t(p[i]) << (8 * i);
    return T(v);
}

std::string lower(std::string s)
{
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool fail(std::string* error, std::string message)
{
    if (error) *error = std::move(message);
    return false;
}

} // namespace

uint64_t fnv1a64(const void* data, size_t size, uint64_t h)
{
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

// --- Reading -------------------------------------------------------------------

bool PackReader::open(const std::filesystem::path& file, std::string* error)
{
    m_file = file;
    m_entries.clear();
    m_byName.clear();
    m_byLowerName.clear();
    m_stream = std::ifstream(file, std::ios::binary);
    if (!m_stream) return fail(error, "can't open " + file.string());
    uint8_t head[kHeaderSize];
    if (!m_stream.read(reinterpret_cast<char*>(head), kHeaderSize) || std::memcmp(head, kMagic, 8) != 0)
        return fail(error, file.string() + " is not a .vepak archive (or another format version)");
    const uint64_t indexOffset = get<uint64_t>(head + 8), indexStored = get<uint64_t>(head + 16), indexSize = get<uint64_t>(head + 24);
    const uint32_t count = get<uint32_t>(head + 32);
    if (indexStored > (1ull << 30) || indexSize > (1ull << 30)) return fail(error, file.string() + ": corrupt index");

    std::vector<uint8_t> stored(static_cast<size_t>(indexStored)), index(static_cast<size_t>(indexSize));
    m_stream.seekg(std::streamoff(indexOffset));
    if (!m_stream.read(reinterpret_cast<char*>(stored.data()), std::streamsize(stored.size())))
        return fail(error, file.string() + ": truncated index");
    if (ZSTD_isError(ZSTD_decompress(index.data(), index.size(), stored.data(), stored.size())))
        return fail(error, file.string() + ": corrupt index");

    const uint8_t* p = index.data();
    const uint8_t* end = p + index.size();
    m_entries.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (end - p < 4) return fail(error, file.string() + ": corrupt index");
        const uint32_t len = get<uint32_t>(p);
        p += 4;
        if (size_t(end - p) < size_t(len) + 33) return fail(error, file.string() + ": corrupt index");
        Entry e;
        e.name.assign(reinterpret_cast<const char*>(p), len);
        p += len;
        e.offset = get<uint64_t>(p);
        e.storedSize = get<uint64_t>(p + 8);
        e.size = get<uint64_t>(p + 16);
        e.method = p[24];
        e.hash = get<uint64_t>(p + 25);
        p += 33;
        m_byName[e.name] = m_entries.size();
        m_byLowerName.emplace(lower(e.name), m_entries.size());
        m_entries.push_back(std::move(e));
    }
    return true;
}

const PackReader::Entry* PackReader::find(const std::string& name) const
{
    if (auto it = m_byName.find(name); it != m_byName.end()) return &m_entries[it->second];
    if (auto it = m_byLowerName.find(lower(name)); it != m_byLowerName.end()) return &m_entries[it->second];
    return nullptr;
}

std::optional<std::vector<uint8_t>> PackReader::read(const Entry& e) const
{
    std::vector<uint8_t> stored(size_t(e.storedSize));
    {
        std::lock_guard lock(m_lock);
        m_stream.clear();
        m_stream.seekg(std::streamoff(e.offset));
        if (!m_stream.read(reinterpret_cast<char*>(stored.data()), std::streamsize(stored.size()))) return std::nullopt;
    }
    if (e.method == 0) return stored;
    std::vector<uint8_t> out(size_t(e.size));
    const size_t n = ZSTD_decompress(out.data(), out.size(), stored.data(), stored.size());
    if (ZSTD_isError(n) || n != out.size()) return std::nullopt;
    return out;
}

// --- Writing -------------------------------------------------------------------

bool PackWriter::begin(const std::filesystem::path& file, int level, std::string* error)
{
    m_file = file;
    m_level = level;
    m_entries.clear();
    m_raw = m_stored = 0;
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    m_out = std::ofstream(file, std::ios::binary | std::ios::trunc);
    if (!m_out) return fail(error, "can't write " + file.string());
    const std::vector<uint8_t> header(kHeaderSize, 0); // rewritten by finish()
    m_out.write(reinterpret_cast<const char*>(header.data()), std::streamsize(header.size()));
    return bool(m_out);
}

bool PackWriter::add(const std::string& name, const void* data, size_t size)
{
    PackReader::Entry e;
    e.name = name;
    e.size = size;
    e.hash = fnv1a64(data, size);
    std::vector<uint8_t> packed;
    if (m_level > 0 && size > 64) {
        packed.resize(ZSTD_compressBound(size));
        const size_t n = ZSTD_compress(packed.data(), packed.size(), data, size, m_level);
        if (!ZSTD_isError(n) && n < size - size / 32) {
            packed.resize(n);
            e.method = 1;
        }
    }
    const void* bytes = e.method == 1 ? static_cast<const void*>(packed.data()) : data;
    e.storedSize = e.method == 1 ? packed.size() : size;
    // 16-byte aligned starts (cheap, and friendly to any later direct mapping).
    static const char zeros[16] = {};
    const auto pos = uint64_t(m_out.tellp());
    if (pos % 16) m_out.write(zeros, std::streamsize(16 - pos % 16));
    e.offset = uint64_t(m_out.tellp());
    m_out.write(static_cast<const char*>(bytes), std::streamsize(e.storedSize));
    m_raw += size;
    m_stored += e.storedSize;
    m_entries.push_back(std::move(e));
    return bool(m_out);
}

bool PackWriter::finish(std::string* error)
{
    std::vector<uint8_t> index;
    for (const PackReader::Entry& e : m_entries) {
        put<uint32_t>(index, uint32_t(e.name.size()));
        index.insert(index.end(), e.name.begin(), e.name.end());
        put<uint64_t>(index, e.offset);
        put<uint64_t>(index, e.storedSize);
        put<uint64_t>(index, e.size);
        index.push_back(e.method);
        put<uint64_t>(index, e.hash);
    }
    std::vector<uint8_t> packed(ZSTD_compressBound(index.size()));
    const size_t n = ZSTD_compress(packed.data(), packed.size(), index.data(), index.size(), std::max(m_level, 3));
    if (ZSTD_isError(n)) return fail(error, "can't compress the index");
    const uint64_t indexOffset = uint64_t(m_out.tellp());
    m_out.write(reinterpret_cast<const char*>(packed.data()), std::streamsize(n));

    std::vector<uint8_t> header(kMagic, kMagic + 8);
    put<uint64_t>(header, indexOffset);
    put<uint64_t>(header, n);
    put<uint64_t>(header, index.size());
    put<uint32_t>(header, uint32_t(m_entries.size()));
    put<uint32_t>(header, 0u);
    m_out.seekp(0);
    m_out.write(reinterpret_cast<const char*>(header.data()), std::streamsize(header.size()));
    m_out.close();
    if (!m_out) return fail(error, "can't write " + m_file.string());
    return true;
}

} // namespace ve
