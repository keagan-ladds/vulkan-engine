#include "ve/assets/IconCook.hpp"

#include "ve/core/BinaryIO.hpp"
#include "ve/core/Engine.hpp"
#include "ve/core/FileSystem.hpp"

#include <cstring>

namespace ve {

namespace {

constexpr char kMagic[8] = {'V', 'E', 'I', 'C', 'O', 'N', '\0', '\0'};

uint64_t fileTime(const std::filesystem::path& file)
{
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(file, ec);
    return ec ? 0 : uint64_t(t.time_since_epoch().count());
}

IconBaker& bakerSlot()
{
    static IconBaker b;
    return b;
}

uint64_t settingsHash(const nlohmann::json& settings)
{
    const std::string s = settings.dump();
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
}

} // namespace

nlohmann::json prefabIconSettings(const std::filesystem::path& prefabFile)
{
    const auto text = vfs::readText(prefabFile);
    if (!text) return nlohmann::json::object();
    const nlohmann::json j = nlohmann::json::parse(*text, nullptr, false);
    if (!j.is_object() || !j.contains("icon") || !j["icon"].is_object()) return nlohmann::json::object();
    return j["icon"];
}

bool isIconPrefab(const std::filesystem::path& file)
{
    if (file.extension() != ".prefab") return false;
    const auto text = vfs::readText(file);
    if (!text || text->find("\"icon\"") == std::string::npos) return false; // (cheap check first)
    const nlohmann::json j = nlohmann::json::parse(*text, nullptr, false);
    return j.is_object() && j.contains("icon") && j["icon"].is_object();
}

std::filesystem::path cookedIconPath(const std::filesystem::path& cacheDir, const std::string& assetPath)
{
    return cacheDir / (assetPath + ".veicon");
}

nlohmann::json makeIconStamp(const std::filesystem::path& prefab, const nlohmann::json& settings,
                             const std::vector<std::filesystem::path>& dependencies)
{
    nlohmann::json stamp = makeCookStamp(prefab, kIconCookerVersion, settingsHash(settings), settings);
    nlohmann::json deps = nlohmann::json::array();
    std::error_code ec;
    for (const std::filesystem::path& d : dependencies) {
        if (!std::filesystem::is_regular_file(d, ec)) continue;
        // Absolute: an icon's dependencies can live anywhere in the asset folders.
        deps.push_back({{"path", d.generic_string()},
                        {"size", uint64_t(std::filesystem::file_size(d, ec))},
                        {"time", fileTime(d)},
                        {"hash", cookHex(hashFileContent(d))}});
    }
    stamp["dependencies"] = std::move(deps);
    return stamp;
}

CookedState checkCookedIcon(const std::filesystem::path& cooked, const std::filesystem::path& prefab, const nlohmann::json& settings)
{
    nlohmann::json stamp;
    if (!readCookedIcon(cooked, &stamp, nullptr, nullptr)) return CookedState::Unusable;
    std::error_code ec;
    const bool haveSource = !prefab.empty() && std::filesystem::exists(prefab, ec);
    const uint64_t hash = settingsHash(settings);
    const CookedState state = checkCookStamp(stamp, prefab, kIconCookerVersion, haveSource ? &hash : nullptr);
    if (state != CookedState::Current || !haveSource) return state;
    for (const nlohmann::json& d : stamp.value("dependencies", nlohmann::json::array())) {
        const std::filesystem::path file = d.value("path", std::string());
        if (!std::filesystem::exists(file, ec)) return CookedState::Stale;
        if (d.value("size", uint64_t(0)) != std::filesystem::file_size(file, ec)) return CookedState::Stale;
        if (d.value("time", uint64_t(0)) == fileTime(file)) continue;
        if (d.value("hash", std::string()) != cookHex(hashFileContent(file))) return CookedState::Stale;
    }
    return CookedState::Current;
}

bool writeCookedIcon(const std::filesystem::path& file, const nlohmann::json& stamp, uint32_t size, const std::vector<uint8_t>& rgba,
                     std::string* error)
{
    if (rgba.size() != size_t(size) * size * 4) {
        if (error) *error = "icon pixels don't match its size";
        return false;
    }
    BinaryWriter w;
    w.putBytes(kMagic, sizeof(kMagic));
    w.put<uint32_t>(kIconCookerVersion);
    w.putString(stamp.dump());
    w.put<uint32_t>(size);
    w.putVector(rgba);
    return writeFileReplacing(file, w.data().data(), w.data().size(), error);
}

bool readCookedIcon(const std::filesystem::path& file, nlohmann::json* stamp, uint32_t* size, std::vector<uint8_t>* rgba)
{
    const auto bytes = vfs::read(file);
    if (!bytes) return false;
    BinaryReader r(*bytes);
    char magic[8] = {};
    for (char& c : magic) c = r.get<char>();
    if (std::memcmp(magic, kMagic, sizeof(kMagic)) != 0 || r.get<uint32_t>() != kIconCookerVersion) return false;
    const std::string stampText = r.getString();
    if (stamp) *stamp = nlohmann::json::parse(stampText, nullptr, false);
    const uint32_t s = r.get<uint32_t>();
    if (size) *size = s;
    if (rgba) {
        *rgba = r.getVector<uint8_t>();
        if (rgba->size() != size_t(s) * s * 4) return false;
    }
    return r.ok();
}

void setIconBaker(IconBaker baker)
{
    bakerSlot() = std::move(baker);
}

const IconBaker& iconBaker()
{
    return bakerSlot();
}

IconCookResult cookIcon(const std::filesystem::path& prefab, const std::filesystem::path& output)
{
    IconCookResult r;
    if (!Engine::exists() || !iconBaker()) {
        r.error = "icons need the renderer (cook them from vecook or the game)";
        return r;
    }
    const nlohmann::json settings = prefabIconSettings(prefab);
    std::vector<uint8_t> rgba;
    std::vector<std::filesystem::path> deps;
    if (!iconBaker()(prefab, settings, r.size, rgba, deps, r.error)) return r;
    if (!writeCookedIcon(output, makeIconStamp(prefab, settings, deps), r.size, rgba, &r.error)) return r;
    r.ok = true;
    std::error_code ec;
    r.bytes = std::filesystem::file_size(output, ec);
    return r;
}

} // namespace ve
