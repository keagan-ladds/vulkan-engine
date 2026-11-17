#pragma once

// =============================================================================
// Log - tiny logging facility built on std::format.
//
//   ve::log::info("Loaded {} meshes in {:.2f} ms", count, ms);
//   ve::log::warn(...);  ve::log::error(...);  ve::log::trace(...);
//
// Output goes to stderr with a coloured level tag. Swap the body of
// ve::log::write() to route logs elsewhere (file, in-game console...).
// =============================================================================

#include <format>
#include <string_view>
#include <utility>

namespace ve::log {

enum class Level { Trace, Info, Warn, Error };

void write(Level level, std::string_view message);
// Logs a warning the first time this exact message is seen (data errors that
// would otherwise repeat every frame or on every rebuild).
void warnOnce(std::string_view message);

template <class... Args>
void trace(std::format_string<Args...> fmt, Args&&... args)
{
    write(Level::Trace, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void info(std::format_string<Args...> fmt, Args&&... args)
{
    write(Level::Info, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void warn(std::format_string<Args...> fmt, Args&&... args)
{
    write(Level::Warn, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void error(std::format_string<Args...> fmt, Args&&... args)
{
    write(Level::Error, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace ve::log
