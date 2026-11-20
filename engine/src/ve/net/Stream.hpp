#pragma once
// =============================================================================
// Byte streams for network messages: write values in order, read them back
// in the same order on the other side.
//
//   ve::net::Writer w;
//   w.write(position); w.write(uint8_t(kind)); w.writeString(prefab);
//   ...
//   ve::net::Reader r(bytes);
//   glm::vec3 p = r.read<glm::vec3>(); auto kind = r.read<uint8_t>(); auto s = r.readString();
//
// Little-endian, no padding. Reading past the end sets failed() and returns
// zeroes (a bad / truncated packet can't crash the reader); check failed()
// before trusting what you read from a remote peer.
// =============================================================================

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <bit>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace ve::net {

// Values are memcpy'd as they are in memory, so "little-endian on the wire"
// holds only on little-endian machines (every platform the engine targets).
// A big-endian port would need byte swaps in write / read.
static_assert(std::endian::native == std::endian::little, "ve::net streams assume a little-endian CPU");

class Writer {
public:
    template <class T>
    void write(const T& v)
    {
        static_assert(std::is_trivially_copyable_v<T>, "write: plain values only (use writeString / writeJson / writeBytes)");
        const auto* p = reinterpret_cast<const uint8_t*>(&v);
        m_data.insert(m_data.end(), p, p + sizeof(T));
    }
    // 1..10 bytes: small numbers stay small (ids, counts, lengths).
    void writeVarint(uint64_t v)
    {
        while (v >= 0x80) {
            m_data.push_back(uint8_t(v) | 0x80);
            v >>= 7;
        }
        m_data.push_back(uint8_t(v));
    }
    void writeString(const std::string& s)
    {
        writeVarint(s.size());
        m_data.insert(m_data.end(), s.begin(), s.end());
    }
    void writeBytes(std::span<const uint8_t> bytes)
    {
        writeVarint(bytes.size());
        m_data.insert(m_data.end(), bytes.begin(), bytes.end());
    }
    // CBOR: compact binary JSON (world snapshots, free-form payloads).
    void writeJson(const nlohmann::json& j) { writeBytes(nlohmann::json::to_cbor(j)); }
    // Rotation in 7 bytes: the largest component is dropped and rebuilt,
    // the other three as 16-bit fixed point (error ~0.0001).
    void writeRotation(const glm::quat& q);

    void append(const Writer& other) { m_data.insert(m_data.end(), other.m_data.begin(), other.m_data.end()); }

    const std::vector<uint8_t>& data() const { return m_data; }
    size_t size() const { return m_data.size(); }
    void clear() { m_data.clear(); }

private:
    std::vector<uint8_t> m_data;
};

class Reader {
public:
    explicit Reader(std::span<const uint8_t> bytes) : m_data(bytes) {}

    template <class T>
    T read()
    {
        static_assert(std::is_trivially_copyable_v<T>);
        T v{};
        if (!take(sizeof(T))) return v;
        std::memcpy(&v, m_data.data() + m_pos - sizeof(T), sizeof(T));
        return v;
    }
    uint64_t readVarint()
    {
        uint64_t v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (!take(1)) return 0;
            const uint8_t b = m_data[m_pos - 1];
            v |= uint64_t(b & 0x7f) << shift;
            if (!(b & 0x80)) return v;
        }
        m_failed = true;
        return 0;
    }
    std::string readString()
    {
        const uint64_t n = readVarint();
        if (!take(n)) return {};
        return std::string(reinterpret_cast<const char*>(m_data.data() + m_pos - n), size_t(n));
    }
    std::span<const uint8_t> readBytes()
    {
        const uint64_t n = readVarint();
        if (!take(n)) return {};
        return m_data.subspan(m_pos - n, size_t(n));
    }
    nlohmann::json readJson()
    {
        auto b = readBytes();
        if (m_failed) return nullptr;
        auto j = nlohmann::json::from_cbor(b.begin(), b.end(), true, false);
        if (j.is_discarded()) m_failed = true;
        return j;
    }
    glm::quat readRotation();

    bool failed() const { return m_failed; }
    bool atEnd() const { return m_pos >= m_data.size(); }
    size_t remaining() const { return m_failed ? 0 : m_data.size() - m_pos; }

private:
    bool take(uint64_t n)
    {
        if (m_failed || n > m_data.size() - m_pos) {
            m_failed = true;
            return false;
        }
        m_pos += size_t(n);
        return true;
    }
    std::span<const uint8_t> m_data;
    size_t m_pos = 0;
    bool m_failed = false;
};

inline void Writer::writeRotation(const glm::quat& qIn)
{
    glm::quat q = glm::normalize(qIn);
    float c[4] = {q.x, q.y, q.z, q.w};
    int largest = 0;
    for (int i = 1; i < 4; ++i)
        if (std::abs(c[i]) > std::abs(c[largest])) largest = i;
    const float sign = c[largest] < 0.0f ? -1.0f : 1.0f; // q and -q are the same rotation
    write(uint8_t(largest));
    for (int i = 0; i < 4; ++i) {
        if (i == largest) continue;
        const float v = glm::clamp(c[i] * sign, -0.70710678f, 0.70710678f) / 0.70710678f; // the others are <= 1/sqrt(2)
        write(int16_t(std::lround(v * 32767.0f)));
    }
}

inline glm::quat Reader::readRotation()
{
    const uint8_t largest = read<uint8_t>();
    float c[4] = {};
    float sum = 0.0f;
    for (int i = 0; i < 4; ++i) {
        if (i == largest) continue;
        c[i] = float(read<int16_t>()) / 32767.0f * 0.70710678f;
        sum += c[i] * c[i];
    }
    if (largest > 3) {
        m_failed = true;
        return {1, 0, 0, 0};
    }
    c[largest] = std::sqrt(std::max(0.0f, 1.0f - sum));
    return glm::normalize(glm::quat(c[3], c[0], c[1], c[2]));
}

} // namespace ve::net
