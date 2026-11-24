#pragma once

// =============================================================================
// Strings - tiny helpers for the "a; b; c" / "name:1, other:2" lists used in
// scene files and inspector fields.
// =============================================================================

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ve::str {

/// Without leading/trailing spaces, tabs and newlines.
inline std::string trim(std::string_view s)
{
    const size_t b = s.find_first_not_of(" \t\n\r");
    if (b == std::string_view::npos) return {};
    const size_t e = s.find_last_not_of(" \t\n\r");
    return std::string(s.substr(b, e - b + 1));
}

/// "a; b ;;c" -> {"a", "b", "c"}: split at `sep`, trimmed, empty items dropped.
inline std::vector<std::string> split(std::string_view s, char sep)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t end = s.find(sep, start);
        if (end == std::string_view::npos) end = s.size();
        if (std::string item = trim(s.substr(start, end - start)); !item.empty()) out.push_back(std::move(item));
        start = end + 1;
    }
    return out;
}

/// Binary data in text files (scene JSON): standard base64 with padding.
inline std::string base64Encode(const void* data, size_t size)
{
    static constexpr char kChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto* b = static_cast<const unsigned char*>(data);
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    for (size_t i = 0; i < size; i += 3) {
        const uint32_t v = uint32_t(b[i]) << 16 | (i + 1 < size ? uint32_t(b[i + 1]) << 8 : 0u) | (i + 2 < size ? uint32_t(b[i + 2]) : 0u);
        out += kChars[(v >> 18) & 63];
        out += kChars[(v >> 12) & 63];
        out += i + 1 < size ? kChars[(v >> 6) & 63] : '=';
        out += i + 2 < size ? kChars[v & 63] : '=';
    }
    return out;
}

inline std::vector<uint8_t> base64Decode(std::string_view text)
{
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        return c == '+' ? 62 : c == '/' ? 63 : -1;
    };
    std::vector<uint8_t> out;
    out.reserve(text.size() / 4 * 3);
    uint32_t acc = 0;
    int bits = 0;
    for (char c : text) {
        const int v = value(c);
        if (v < 0) continue; // padding, whitespace
        acc = acc << 6 | uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(uint8_t(acc >> bits));
        }
    }
    return out;
}

} // namespace ve::str
