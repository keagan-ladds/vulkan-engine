#include "ve/assets/Cook.hpp"

#include <cstdio>
#include <fstream>
#include <vector>

namespace ve {

namespace {

uint64_t fileTime(const std::filesystem::path& file)
{
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(file, ec);
    return ec ? 0 : uint64_t(t.time_since_epoch().count());
}

} // namespace

uint64_t hashFileContent(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in) return 0;
    uint64_t h = 1469598103934665603ull;
    std::vector<char> buf(1 << 16);
    while (in) {
        in.read(buf.data(), std::streamsize(buf.size()));
        const std::streamsize n = in.gcount();
        for (std::streamsize i = 0; i < n; ++i) {
            h ^= uint8_t(buf[size_t(i)]);
            h *= 1099511628211ull;
        }
    }
    return h;
}

std::string cookHex(uint64_t v)
{
    char b[17];
    std::snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(v));
    return b;
}

nlohmann::json makeCookStamp(const std::filesystem::path& source, uint32_t cookerVersion, uint64_t settingsHash,
                             const nlohmann::json& settings)
{
    std::error_code ec;
    return {{"version", cookerVersion},
            {"settingsHash", cookHex(settingsHash)},
            {"sourceSize", uint64_t(std::filesystem::file_size(source, ec))},
            {"sourceTime", fileTime(source)},
            {"sourceHash", cookHex(hashFileContent(source))},
            {"settings", settings}};
}

CookedState checkCookStamp(const nlohmann::json& stamp, const std::filesystem::path& source, uint32_t cookerVersion,
                           const uint64_t* settingsHash)
{
    if (!stamp.is_object() || stamp.value("version", 0u) != cookerVersion) return CookedState::Stale;
    std::error_code ec;
    if (source.empty() || !std::filesystem::exists(source, ec)) return CookedState::Current; // shipped build: the cooked file is all there is
    if (settingsHash && stamp.value("settingsHash", std::string()) != cookHex(*settingsHash)) return CookedState::Stale;
    if (stamp.value("sourceSize", uint64_t(0)) != std::filesystem::file_size(source, ec)) return CookedState::Stale;
    if (stamp.value("sourceTime", uint64_t(0)) == fileTime(source)) return CookedState::Current;
    // Same size, another time (a checkout, a copy): compare the content.
    return stamp.value("sourceHash", std::string()) == cookHex(hashFileContent(source)) ? CookedState::Current : CookedState::Stale;
}

bool writeFileReplacing(const std::filesystem::path& file, const void* data, size_t size, std::string* error)
{
    std::filesystem::path tmp = file;
    tmp += ".tmp";
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(static_cast<const char*>(data), std::streamsize(size));
        if (!out) {
            if (error) *error = "can't write " + tmp.string();
            return false;
        }
    }
    std::filesystem::rename(tmp, file, ec);
    if (ec) {
        // Some runtimes (older MinGW libstdc++) won't rename over an existing file.
        std::filesystem::remove(file, ec);
        std::filesystem::rename(tmp, file, ec);
    }
    if (ec) {
        std::filesystem::remove(tmp, ec);
        if (error) *error = "can't replace " + file.string();
        return false;
    }
    return true;
}

} // namespace ve
