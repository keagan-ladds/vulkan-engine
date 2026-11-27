#include "ve/ui/PrefabIcon.hpp"

#include "ve/assets/AssetManager.hpp"
#include "ve/assets/IconCook.hpp"
#include "ve/assets/ModelCook.hpp"
#include "ve/core/Engine.hpp"
#include "ve/core/Log.hpp"
#include "ve/render/Renderer.hpp"
#include "ve/render/Texture.hpp"
#include "ve/scene/Prefab.hpp"
#include "ve/scene/Scene.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <unordered_map>

namespace ve::ui {

namespace {

PrefabIconStats& stats()
{
    static PrefabIconStats s;
    return s;
}

std::unordered_map<std::string, std::shared_ptr<Texture>>& cache()
{
    static std::unordered_map<std::string, std::shared_ptr<Texture>> c;
    return c;
}

// The asset files a prefab names (models, textures, materials, other prefabs):
// what its icon depends on besides the prefab file itself.
void collectDependencies(const Json& j, AssetManager& assets, std::vector<std::filesystem::path>& out)
{
    if (j.is_object() || j.is_array()) {
        for (const Json& v : j) collectDependencies(v, assets, out);
        return;
    }
    if (!j.is_string()) return;
    const std::string s = j.get<std::string>();
    std::string ext = std::filesystem::path(s).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    static const char* kAssetExts[] = {".gltf", ".glb", ".png", ".jpg", ".jpeg", ".tga", ".material", ".prefab"};
    if (std::none_of(std::begin(kAssetExts), std::end(kAssetExts), [&](const char* e) { return ext == e; })) return;
    try {
        const std::filesystem::path file = assets.resolve(s);
        out.push_back(file);
        if (ext == ".gltf")
            for (const auto& d : modelDependencies(file)) out.push_back(d); // its .bin buffers
    } catch (const std::exception&) {
    }
}

} // namespace

IconView iconViewFromJson(const nlohmann::json& settings, uint32_t& size)
{
    IconView v;
    size = 192;
    if (!settings.is_object()) return v;
    size = std::clamp(settings.value("size", 192u), 16u, 1024u);
    v.yawDegrees = settings.value("yaw", v.yawDegrees);
    v.pitchDegrees = settings.value("pitch", v.pitchDegrees);
    v.padding = std::clamp(settings.value("padding", v.padding), 0.0f, 0.45f);
    if (const auto it = settings.find("sun"); it != settings.end() && it->is_array() && it->size() == 3)
        v.sunDirection = {(*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>()};
    return v;
}

bool renderPrefabIcon(const std::filesystem::path& prefab, const nlohmann::json& settings, uint32_t& size, std::vector<uint8_t>& rgba,
                      std::vector<std::filesystem::path>& dependencies, std::string& error)
{
    if (!Engine::exists()) {
        error = "no Engine";
        return false;
    }
    Engine& engine = Engine::get();
    // A private scene: the game's scene never sees these objects, the renderer
    // never draws them; their start() builds their meshes as usual.
    Scene scene;
    GameObject* root = prefab::instantiate(scene, prefab.generic_string());
    if (!root) {
        error = "can't instantiate " + prefab.generic_string();
        return false;
    }
    for (int pass = 0; pass < 4; ++pass) scene.startPending(); // (start() may spawn children: they start next pass)
    const std::vector<IconPart> parts = iconParts(*root);
    if (parts.empty()) {
        error = prefab.generic_string() + " draws nothing (no MeshRenderer)";
        return false;
    }
    const IconView view = iconViewFromJson(settings, size);
    rgba = renderIconPixels(engine.renderer(), parts, size, view);
    if (rgba.empty()) {
        error = "nothing visible to frame";
        return false;
    }
    if (const Json* pj = prefab::load(prefab.generic_string())) collectDependencies(*pj, engine.assets(), dependencies);
    return true; // (the scene and its objects go here: onDestroy releases their bodies)
}

std::shared_ptr<Texture> prefabIcon(const std::string& prefabPath)
{
    if (auto it = cache().find(prefabPath); it != cache().end()) return it->second;
    if (!Engine::exists()) return nullptr;
    AssetManager& assets = Engine::get().assets();
    Renderer& renderer = Engine::get().renderer();
    std::shared_ptr<Texture> tex;
    auto fromPixels = [&](uint32_t size, const std::vector<uint8_t>& rgba) {
        return Texture::create2D(renderer, size, size, VK_FORMAT_R8G8B8A8_SRGB, rgba.data(), rgba.size(), true, SamplerType::LinearClamp,
                                 "icon " + prefabPath);
    };

    if (assets.exists(prefabPath + ".png")) {
        tex = assets.loadTexture(prefabPath + ".png"); // hand-made
        ++stats().handMade;
    } else {
        std::filesystem::path source;
        try {
            source = assets.resolve(prefabPath);
        } catch (const std::exception&) {
        }
        const nlohmann::json settings = source.empty() ? nlohmann::json::object() : prefabIconSettings(source);
        const AssetManager::CookOptions& cook = assets.cooking();
        const std::filesystem::path cooked = cook.cacheDir.empty() ? std::filesystem::path() : cookedIconPath(cook.cacheDir, prefabPath);
        uint32_t size = 0;
        std::vector<uint8_t> rgba;
        if (cook.useCooked && !cooked.empty() && checkCookedIcon(cooked, source, settings) == CookedState::Current &&
            readCookedIcon(cooked, nullptr, &size, &rgba)) {
            tex = fromPixels(size, rgba);
            ++stats().cooked;
        } else {
            const auto t0 = std::chrono::steady_clock::now();
            std::vector<std::filesystem::path> deps;
            std::string error;
            if (renderPrefabIcon(source.empty() ? std::filesystem::path(prefabPath) : source, settings, size, rgba, deps, error)) {
                tex = fromPixels(size, rgba);
                ++stats().rendered;
                std::string note;
                if (cook.cookOnDemand && !cooked.empty() && !source.empty()) {
                    std::string writeError;
                    note = writeCookedIcon(cooked, makeIconStamp(source, settings, deps), size, rgba, &writeError) ? ", cached for next time"
                                                                                                                 : ", not cached: " + writeError;
                }
                log::info("Icon of {} rendered ({}x{}) in {:.1f} ms{}", prefabPath, size, size,
                          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), note);
            } else {
                log::warnOnce("Icon of " + prefabPath + ": " + error);
            }
        }
    }
    cache()[prefabPath] = tex;
    return tex;
}

const PrefabIconStats& prefabIconStats()
{
    return stats();
}

void clearPrefabIcons()
{
    cache().clear();
}

void registerPrefabIconBaker()
{
    setIconBaker(renderPrefabIcon);
}

} // namespace ve::ui
