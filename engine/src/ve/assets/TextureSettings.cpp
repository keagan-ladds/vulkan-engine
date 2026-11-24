#include "ve/assets/TextureSettings.hpp"

#include "ve/core/Log.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <string_view>

namespace ve {

namespace {

constexpr std::array<std::string_view, 5> kNormalWords{"normal", "nrm", "nor", "norm", "n"};
constexpr std::array<std::string_view, 23> kDataWords{
    "rough", "roughness", "metal", "metallic", "metalness", "ao", "occlusion", "orm",
    "arm", "rma", "mr", "mask", "height", "disp", "displacement", "detail",
    "noise", "data", "lut", "opacity", "gloss", "spec", "specular"};

template <class E>
bool enumFromString(const nlohmann::json& j, std::initializer_list<std::pair<const char*, E>> names, E& out)
{
    if (!j.is_string()) return false;
    std::string s = j.get<std::string>();
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    for (const auto& [name, value] : names) {
        std::string n = name;
        for (char& c : n) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (s == n) {
            out = value;
            return true;
        }
    }
    return false;
}

} // namespace

const char* toString(TextureType type)
{
    switch (type) {
    case TextureType::Data: return "Data";
    case TextureType::Normal: return "Normal";
    default: return "Color";
    }
}

SamplerType TextureSettings::sampler() const
{
    if (filter == TextureFilter::Nearest) return wrap == TextureWrap::Clamp ? SamplerType::NearestClamp : SamplerType::NearestRepeat;
    return wrap == TextureWrap::Clamp ? SamplerType::LinearClamp : SamplerType::LinearRepeat;
}

std::filesystem::path TextureSettings::settingsFile(const std::filesystem::path& imageFile)
{
    std::filesystem::path p = imageFile;
    p += ".texture";
    return p;
}

TextureType TextureSettings::typeFromName(const std::filesystem::path& imageFile)
{
    // Words of the stem, last first; skip size tags ("2k", "4096").
    std::string stem = imageFile.stem().string();
    for (char& c : stem) c = char(std::tolower(static_cast<unsigned char>(c)));
    size_t end = stem.size();
    while (end > 0) {
        size_t begin = stem.find_last_of("_-. ", end - 1);
        begin = begin == std::string::npos ? 0 : begin + 1;
        const std::string_view word(stem.data() + begin, end - begin);
        end = begin > 0 ? begin - 1 : 0;
        if (word.empty()) continue;
        const bool sizeTag = std::all_of(word.begin(), word.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }) ||
                             (word.size() >= 2 && word.back() == 'k' &&
                              std::all_of(word.begin(), word.end() - 1, [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }));
        if (sizeTag) continue;
        if (std::find(kNormalWords.begin(), kNormalWords.end(), word) != kNormalWords.end()) return TextureType::Normal;
        if (std::find(kDataWords.begin(), kDataWords.end(), word) != kDataWords.end()) return TextureType::Data;
        return TextureType::Color; // the last real word decides
    }
    return TextureType::Color;
}

TextureSettings TextureSettings::forImage(const std::filesystem::path& imageFile)
{
    TextureSettings s;
    s.type = typeFromName(imageFile);
    const std::filesystem::path file = settingsFile(imageFile);
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) return s;
    std::ifstream in(file, std::ios::binary);
    const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (!j.is_object()) {
        log::warn("Texture settings {}: not a JSON object (using defaults)", file.string());
        return s;
    }
    s.applyJson(j);
    return s;
}

nlohmann::json TextureSettings::toJson() const
{
    return {{"type", toString(type)},
            {"mipmaps", mipmaps},
            {"compress", compress},
            {"maxSize", maxSize},
            {"filter", filter == TextureFilter::Nearest ? "Nearest" : "Linear"},
            {"wrap", wrap == TextureWrap::Clamp ? "Clamp" : "Repeat"}};
}

void TextureSettings::applyJson(const nlohmann::json& j)
{
    if (!j.is_object()) return;
    if (auto it = j.find("type"); it != j.end() &&
        !enumFromString<TextureType>(*it, {{"Color", TextureType::Color}, {"Colour", TextureType::Color}, {"Data", TextureType::Data},
                                           {"Linear", TextureType::Data}, {"Normal", TextureType::Normal}}, type))
        log::warn("Texture settings: unknown type {} (Color, Data or Normal)", it->dump());
    if (auto it = j.find("mipmaps"); it != j.end() && it->is_boolean()) mipmaps = it->get<bool>();
    if (auto it = j.find("compress"); it != j.end() && it->is_boolean()) compress = it->get<bool>();
    if (auto it = j.find("maxSize"); it != j.end() && it->is_number()) maxSize = uint32_t(std::max(0, it->get<int>()));
    if (auto it = j.find("filter"); it != j.end())
        enumFromString<TextureFilter>(*it, {{"Linear", TextureFilter::Linear}, {"Nearest", TextureFilter::Nearest}}, filter);
    if (auto it = j.find("wrap"); it != j.end())
        enumFromString<TextureWrap>(*it, {{"Repeat", TextureWrap::Repeat}, {"Clamp", TextureWrap::Clamp}}, wrap);
}

uint64_t TextureSettings::cookHash() const
{
    // FNV-1a over the fields that change the cooked bytes.
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (8 * i)) & 0xFF;
            h *= 1099511628211ull;
        }
    };
    mix(uint64_t(type));
    mix(mipmaps);
    mix(compress);
    mix(maxSize);
    return h;
}

} // namespace ve
