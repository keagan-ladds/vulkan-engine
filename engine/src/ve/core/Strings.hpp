#pragma once

// =============================================================================
// Strings - tiny helpers for the "a; b; c" / "name:1, other:2" lists used in
// scene files and inspector fields.
// =============================================================================

#include <string>
#include <string_view>
#include <vector>

namespace ve::str {

// Without leading/trailing spaces, tabs and newlines.
inline std::string trim(std::string_view s)
{
    const size_t b = s.find_first_not_of(" \t\n\r");
    if (b == std::string_view::npos) return {};
    const size_t e = s.find_last_not_of(" \t\n\r");
    return std::string(s.substr(b, e - b + 1));
}

// "a; b ;;c" -> {"a", "b", "c"}: split at `sep`, trimmed, empty items dropped.
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

} // namespace ve::str
