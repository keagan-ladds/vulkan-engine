#pragma once

// =============================================================================
// BinaryIO - minimal little-endian binary writer / reader for engine file
// formats (cooked models, derived data). Values are copied as they are in
// memory, so only plain types (numbers, glm vectors, POD structs without
// pointers) go through put / get; strings and vectors carry their length.
//
//   BinaryWriter w;  w.put<uint32_t>(3);  w.putString("name");  w.putVector(vertices);
//   BinaryReader r(bytes);  auto n = r.get<uint32_t>();  auto s = r.getString();
//   if (!r.ok()) ...  // ran past the end: everything read since is zero / empty
// =============================================================================

#include <bit>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace ve {

static_assert(std::endian::native == std::endian::little, "BinaryIO assumes a little-endian CPU");

/// Appends values to a byte buffer.
class BinaryWriter {
public:
    /// Appends a plain value's bytes.
    template <class T>
        requires std::is_trivially_copyable_v<T>
    void put(const T& v)
    {
        putBytes(&v, sizeof(T));
    }
    /// Appends raw bytes.
    void putBytes(const void* data, size_t size)
    {
        const auto* p = static_cast<const uint8_t*>(data);
        m_data.insert(m_data.end(), p, p + size);
    }
    /// Appends a string (u64 length + bytes).
    void putString(const std::string& s)
    {
        put<uint64_t>(s.size());
        putBytes(s.data(), s.size());
    }
    /// Appends a vector of plain values (u64 count + their bytes).
    template <class T>
        requires std::is_trivially_copyable_v<T>
    void putVector(const std::vector<T>& v)
    {
        put<uint64_t>(v.size());
        putBytes(v.data(), v.size() * sizeof(T));
    }
    /// The bytes written so far.
    const std::vector<uint8_t>& data() const { return m_data; }
    /// Takes the buffer (the writer is empty afterwards).
    std::vector<uint8_t> take() { return std::move(m_data); }

private:
    std::vector<uint8_t> m_data;
};

/// Reads values back in the order a BinaryWriter wrote them. Reading past the
/// end sets !ok() and returns zero / empty values from then on.
class BinaryReader {
public:
    /// Reads from `bytes` (not copied: they must outlive the reader).
    explicit BinaryReader(std::span<const uint8_t> bytes) : m_data(bytes) {}

    /// Reads a plain value.
    template <class T>
        requires std::is_trivially_copyable_v<T>
    T get()
    {
        T v{};
        if (take(sizeof(T))) std::memcpy(&v, m_data.data() + m_pos - sizeof(T), sizeof(T));
        return v;
    }
    /// Reads a string written by putString.
    std::string getString()
    {
        const uint64_t n = get<uint64_t>();
        if (!take(n)) return {};
        return std::string(reinterpret_cast<const char*>(m_data.data() + m_pos - n), size_t(n));
    }
    /// Reads a vector written by putVector.
    template <class T>
        requires std::is_trivially_copyable_v<T>
    std::vector<T> getVector()
    {
        const uint64_t n = get<uint64_t>();
        if (n > remaining() / (sizeof(T) ? sizeof(T) : 1) || !take(n * sizeof(T))) {
            m_ok = false;
            return {};
        }
        std::vector<T> v(static_cast<size_t>(n));
        std::memcpy(v.data(), m_data.data() + m_pos - n * sizeof(T), size_t(n) * sizeof(T));
        return v;
    }
    /// Whether every read so far was within the data.
    bool ok() const { return m_ok; }
    /// Bytes not read yet.
    size_t remaining() const { return m_ok ? m_data.size() - m_pos : 0; }

private:
    bool take(uint64_t n)
    {
        if (!m_ok || n > m_data.size() - m_pos) {
            m_ok = false;
            return false;
        }
        m_pos += size_t(n);
        return true;
    }
    std::span<const uint8_t> m_data;
    size_t m_pos = 0;
    bool m_ok = true;
};

} // namespace ve
