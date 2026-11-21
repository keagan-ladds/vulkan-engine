#pragma once

// =============================================================================
// Env - reading environment variables used as test / debug switches
// (VE_HEADLESS, VE_BENCH, a game's own MYGAME_TIME...).
//
//   if (ve::env::flag("VE_HEADLESS")) ...          // set and not "0"/"false"/"off"/""
//   const float t = ve::env::number("MYGAME_TIME", -1.0f);
//   if (auto s = ve::env::get("MYGAME_POSE")) parse(*s);
// =============================================================================

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace ve::env {

// The variable's value, or nullopt if it isn't set.
inline std::optional<std::string_view> get(const char* name)
{
    const char* v = std::getenv(name);
    if (!v) return std::nullopt;
    return std::string_view(v);
}

// True when set to anything but "", "0", "false", "off" or "no".
inline bool flag(const char* name, bool fallback = false)
{
    const auto v = get(name);
    if (!v) return fallback;
    return !(v->empty() || *v == "0" || *v == "false" || *v == "off" || *v == "no");
}

// The value parsed as a number, or `fallback` if unset or not a number.
inline float number(const char* name, float fallback)
{
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    char* end = nullptr;
    const float f = std::strtof(v, &end);
    return end == v ? fallback : f;
}

inline int integer(const char* name, int fallback)
{
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    char* end = nullptr;
    const long i = std::strtol(v, &end, 10);
    return end == v ? fallback : int(i);
}

} // namespace ve::env
