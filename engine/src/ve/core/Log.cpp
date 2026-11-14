#include "ve/core/Log.hpp"

#include <cstdio>

namespace ve::log {

void write(Level level, std::string_view message)
{
    // ANSI colour codes: grey, default, yellow, red.
    const char* tag = "";
    switch (level) {
    case Level::Trace: tag = "\033[90m[trace]\033[0m "; break;
    case Level::Info:  tag = "\033[36m[info]\033[0m  "; break;
    case Level::Warn:  tag = "\033[33m[warn]\033[0m  "; break;
    case Level::Error: tag = "\033[31m[error]\033[0m "; break;
    }
    std::fprintf(stderr, "%s%.*s\n", tag, static_cast<int>(message.size()), message.data());
}

} // namespace ve::log
