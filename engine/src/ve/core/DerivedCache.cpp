#include "ve/core/DerivedCache.hpp"

#include "ve/core/Log.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>

namespace ve::derived {

namespace {
std::mutex g_mutex;
std::filesystem::path g_directory;
} // namespace

Hasher& Hasher::add(const void* data, size_t size)
{
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        m_hash ^= p[i];
        m_hash *= 1099511628211ull;
    }
    return *this;
}

Hasher& Hasher::add(std::string_view text)
{
    const uint64_t n = text.size();
    add(&n, sizeof(n));
    return add(text.data(), text.size());
}

void setDirectory(const std::filesystem::path& directory)
{
    std::lock_guard lock(g_mutex);
    g_directory = directory;
}

const std::filesystem::path& directory()
{
    return g_directory; // (set once at startup, before any worker uses it)
}

std::filesystem::path path(std::string_view kind, uint64_t key, std::string_view extension)
{
    if (g_directory.empty()) return {};
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(key));
    return g_directory / std::string(kind) / (std::string(hex) + std::string(extension));
}

bool writeFile(const std::filesystem::path& file, const void* data, size_t size)
{
    if (file.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const std::filesystem::path tmp = file.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        if (out) out.write(static_cast<const char*>(data), std::streamsize(size));
        if (!out) {
            log::warnOnce("Derived data cache: can't write " + file.parent_path().string() + " (results are recomputed next run)");
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    std::filesystem::rename(tmp, file, ec);
    if (ec) {
        std::filesystem::remove(file, ec);
        std::filesystem::rename(tmp, file, ec);
    }
    if (ec) return false;
    ++stats().writes;
    return true;
}

Stats& stats()
{
    static Stats s;
    return s;
}

bool bakeMode()
{
    static const bool on = [] {
        const char* v = std::getenv("VE_BAKE");
        return v && *v && v[0] != '0';
    }();
    return on;
}

} // namespace ve::derived
