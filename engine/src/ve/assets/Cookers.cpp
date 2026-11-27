#include "ve/assets/Cookers.hpp"

#include "ve/assets/IconCook.hpp"

#include "ve/assets/AudioCook.hpp"
#include "ve/assets/ModelCook.hpp"
#include "ve/assets/TextureCook.hpp"

#include <cstdio>

namespace ve {

namespace {

const char* formatName(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_BC4_UNORM_BLOCK: return "BC4";
    case VK_FORMAT_BC5_UNORM_BLOCK: return "BC5";
    case VK_FORMAT_BC7_UNORM_BLOCK: return "BC7";
    case VK_FORMAT_BC7_SRGB_BLOCK: return "BC7 sRGB";
    case VK_FORMAT_R8G8B8A8_SRGB: return "RGBA8 sRGB";
    case VK_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
    default: return "?";
    }
}

Cooker textureCooker()
{
    Cooker c;
    c.name = "texture";
    c.settingsExtensions = {".texture"};
    c.handles = [](const std::filesystem::path& source) { return isCookableImage(source); };
    c.cookedPath = cookedTexturePath;
    c.check = [](const std::filesystem::path& cooked, const std::filesystem::path& source) {
        const TextureSettings settings = TextureSettings::forImage(source);
        Ktx2Texture header;
        if (!readKtx2(cooked, header, true)) return CookedState::Unusable;
        return checkCookedTexture(header, source, &settings);
    };
    c.cook = [](const std::filesystem::path& source, const std::filesystem::path& cooked, const std::atomic<bool>* cancelled) {
        const TextureSettings settings = TextureSettings::forImage(source);
        const TextureCookResult r = cookTexture(source, settings, cooked, cancelled);
        char summary[160];
        std::snprintf(summary, sizeof(summary), "%-6s %-10s %5ux%-5u %2u mips", toString(settings.type), formatName(r.format), r.width,
                      r.height, r.levels);
        return Cooker::Result{r.ok, r.error, summary, r.bytes};
    };
    return c;
}

Cooker audioCooker()
{
    Cooker c;
    c.name = "audio";
    c.settingsExtensions = {".audio"};
    c.handles = [](const std::filesystem::path& source) {
        return isCookableAudio(source) && AudioSettings::forFile(source).compress;
    };
    c.cookedPath = cookedAudioPath;
    c.check = [](const std::filesystem::path& cooked, const std::filesystem::path& source) {
        const AudioSettings settings = AudioSettings::forFile(source);
        return checkCookedAudio(cooked, source, &settings);
    };
    c.cook = [](const std::filesystem::path& source, const std::filesystem::path& cooked, const std::atomic<bool>* cancelled) {
        const AudioCookResult r = cookAudio(source, AudioSettings::forFile(source), cooked, cancelled);
        char summary[160];
        std::snprintf(summary, sizeof(summary), "%-6s %-10s %5.1f s %u ch %5u Hz", "Sound", "Vorbis", r.seconds, r.channels, r.sampleRate);
        return Cooker::Result{r.ok, r.error, summary, r.bytes};
    };
    return c;
}

Cooker modelCooker()
{
    Cooker c;
    c.name = "model";
    c.settingsExtensions = {".model"};
    c.handles = [](const std::filesystem::path& source) { return isCookableModel(source); };
    c.cookedPath = cookedModelPath;
    c.check = [](const std::filesystem::path& cooked, const std::filesystem::path& source) {
        return checkCookedModel(cooked, source, ModelSettings::forModel(source));
    };
    c.cook = [](const std::filesystem::path& source, const std::filesystem::path& cooked, const std::atomic<bool>* cancelled) {
        const ModelCookResult r = cookModel(source, ModelSettings::forModel(source), cooked, cancelled);
        char summary[160];
        std::snprintf(summary, sizeof(summary), "%-6s %-10s %3u meshes, %u LOD levels, %u impostors", "Model", "vemodel", r.meshes,
                      r.generatedLevels, r.impostors);
        return Cooker::Result{r.ok, r.error, summary, r.bytes};
    };
    c.needsRenderer = [](const std::filesystem::path& source) { return ModelSettings::forModel(source).generatesLods(); };
    c.dependencies = modelDependencies;
    return c;
}

Cooker iconCooker()
{
    Cooker c;
    c.name = "icon";
    c.handles = [](const std::filesystem::path& source) { return isIconPrefab(source); };
    c.cookedPath = cookedIconPath;
    c.check = [](const std::filesystem::path& cooked, const std::filesystem::path& source) {
        return checkCookedIcon(cooked, source, prefabIconSettings(source));
    };
    c.cook = [](const std::filesystem::path& source, const std::filesystem::path& cooked, const std::atomic<bool>*) {
        const IconCookResult r = cookIcon(source, cooked);
        char summary[160];
        std::snprintf(summary, sizeof(summary), "%-6s %-10s %ux%u", "Icon", "veicon", r.size, r.size);
        return Cooker::Result{r.ok, r.error, summary, r.bytes};
    };
    c.needsRenderer = [](const std::filesystem::path&) { return true; };
    c.unsupportedHere = iconUnsupportedHere;
    c.shipSource = true;
    return c;
}

std::vector<Cooker>& registry()
{
    static std::vector<Cooker> list{textureCooker(), audioCooker(), modelCooker(), iconCooker()};
    return list;
}

} // namespace

const std::vector<Cooker>& cookers()
{
    return registry();
}

const Cooker* cookerFor(const std::filesystem::path& source)
{
    for (const Cooker& c : registry())
        if (c.handles(source)) return &c;
    return nullptr;
}

bool isCookerSettingsFile(const std::filesystem::path& file)
{
    const std::string ext = file.extension().string();
    for (const Cooker& c : registry())
        for (const std::string& e : c.settingsExtensions)
            if (ext == e) return true;
    return false;
}

void registerCooker(Cooker cooker)
{
    registry().push_back(std::move(cooker));
}

} // namespace ve
