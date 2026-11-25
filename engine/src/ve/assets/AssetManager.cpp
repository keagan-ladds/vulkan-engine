#include "ve/assets/AssetManager.hpp"

#include "ve/assets/DataAsset.hpp"
#include "ve/assets/AudioCook.hpp"
#include "ve/assets/Cookers.hpp"
#include "ve/assets/ModelCook.hpp"
#include "ve/assets/ModelData.hpp"
#include "ve/assets/MaterialAsset.hpp"
#include "ve/assets/TextureCook.hpp"
#include "ve/gfx/Device.hpp"
#include "ve/audio/Audio.hpp"
#include "ve/assets/Model.hpp"
#include "ve/core/FileSystem.hpp"
#include "ve/core/Log.hpp"
#include "ve/render/Renderer.hpp"
#include "ve/render/Texture.hpp"

#include <stb_image.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>

namespace ve {

void AssetManager::init(Renderer& renderer, std::vector<std::filesystem::path> searchPaths)
{
    m_renderer = &renderer;
    m_searchPaths = std::move(searchPaths);
}

void AssetManager::clear()
{
    m_textures.clear();
    m_models.clear();
    m_audio.clear();
    m_materials.clear();
    m_materialShaders.clear();
    m_resolved.clear(); // (files may have moved meanwhile)
}

void AssetManager::addSearchPath(const std::filesystem::path& path)
{
    m_searchPaths.push_back(path);
    m_resolved.clear(); // an earlier-found file may now be shadowed
}

const std::filesystem::path& AssetManager::resolveCached(const std::filesystem::path& path)
{
    // Cache hits in the load functions skip the filesystem (exists + canonical
    // per search path). Only successful lookups are remembered.
    std::string key = path.generic_string();
    if (auto it = m_resolved.find(key); it != m_resolved.end()) return it->second;
    return m_resolved.emplace(std::move(key), resolve(path)).first->second;
}

// (Through the virtual file system: files on disk, or in mounted .vepak
// archives in a packaged game, FileSystem.hpp.)
bool AssetManager::exists(const std::filesystem::path& path) const
{
    if (path.is_absolute()) return vfs::exists(path);
    for (const auto& root : m_searchPaths) {
        if (vfs::exists(root / path)) return true;
    }
    // A packaged game ships cookable files only as their cooked copies.
    if (const Cooker* cooker = m_cook.cacheDir.empty() ? nullptr : cookerFor(path))
        return vfs::exists(cooker->cookedPath(m_cook.cacheDir, path.generic_string()));
    return false;
}

ImagePixels AssetManager::loadPixels(const std::filesystem::path& path) const
{
    std::filesystem::path source;
    try {
        source = resolve(path);
    } catch (const std::exception&) {
        // Only the cooked copy (a packaged game): decode its full-size level.
        if (m_cook.cacheDir.empty() || !isCookableImage(path)) throw;
        const std::string name = path.is_absolute() ? assetName(path) : path.generic_string();
        Ktx2Texture cooked;
        std::string error;
        if (!readKtx2(cookedTexturePath(m_cook.cacheDir, name), cooked, false, &error))
            throw std::runtime_error("Asset not found: " + path.string() + " (no source image or readable cooked copy)");
        uint32_t w = 0, h = 0;
        ImagePixels img;
        if (!decodeCookedTexture(cooked, 0, w, h, img.rgba)) throw std::runtime_error("Can't decode the cooked copy of " + name);
        img.width = int(w);
        img.height = int(h);
        return img;
    }
    return loadImagePixels(source);
}

std::filesystem::path AssetManager::resolve(const std::filesystem::path& path) const
{
    // Canonical when it's on disk (one key per file: the caches), else
    // normalized the same way (an archive entry).
    auto normalized = [](const std::filesystem::path& p) {
        std::error_code ec;
        const std::filesystem::path c = std::filesystem::weakly_canonical(p, ec);
        return ec ? p.lexically_normal() : c;
    };
    if (path.is_absolute()) {
        if (vfs::exists(path)) return normalized(path);
    } else {
        for (const auto& root : m_searchPaths) {
            auto p = root / path;
            if (vfs::exists(p)) return normalized(p);
        }
    }
    throw std::runtime_error("Asset not found: " + path.string());
}

std::string AssetManager::assetName(const std::filesystem::path& file) const
{
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(file, ec);
    for (const auto& root : m_searchPaths) {
        const auto r = std::filesystem::weakly_canonical(root, ec);
        const auto rel = canonical.lexically_relative(r);
        if (!rel.empty() && *rel.begin() != "..") return rel.generic_string();
    }
    return file.generic_string();
}

std::shared_ptr<Texture> AssetManager::loadTexture(const std::filesystem::path& path)
{
    // The source image, if it's there (a shipped build may only have the cooked copy).
    std::filesystem::path source;
    std::string name;
    try {
        source = resolveCached(path);
        name = assetName(source);
    } catch (const std::exception&) {
        // No source (a packaged game): its cooked copy, by asset name. An
        // absolute path (a glTF's image, next to the model) is named relative
        // to the search path it's under.
        if (m_cook.cacheDir.empty()) throw;
        name = path.is_absolute() ? assetName(path) : path.generic_string();
        if (std::filesystem::path(name).is_absolute()) throw; // (outside the asset folders)
    }
    // One texture per file: its settings decide how it's loaded, not the caller.
    const std::string key = source.empty() ? "cooked:" + name : source.string();
    if (auto it = m_textures.find(key); it != m_textures.end()) return it->second;

    TextureSettings settings = source.empty() ? TextureSettings{} : TextureSettings::forImage(source);
    std::shared_ptr<Texture> tex;
    const bool cookable = isCookableImage(path);
    if (cookable && m_cook.useCooked && !m_cook.cacheDir.empty() && !m_renderer->device().features().textureCompressionBC) {
        if (!m_warnedNoBC) log::warn("Cooked textures off: this GPU has no BC texture compression");
        m_warnedNoBC = true;
    } else if (cookable && m_cook.useCooked && !m_cook.cacheDir.empty()) {
        const std::filesystem::path cooked = cookedTexturePath(m_cook.cacheDir, name);
        Ktx2Texture header;
        CookedState state = CookedState::Unusable;
        if (readKtx2(cooked, header, true))
            state = checkCookedTexture(header, source, source.empty() ? nullptr : &settings, source.empty() ? &settings : nullptr);
        if (state != CookedState::Current && !source.empty() && m_cook.cookOnDemand && m_cook.synchronous) {
            const TextureCookResult r = cookTexture(source, settings, cooked);
            if (r.ok) state = CookedState::Current;
            else log::warn("Cooking {} failed: {}", name, r.error);
        }
        Ktx2Texture full;
        std::string error;
        if (state == CookedState::Current && readKtx2(cooked, full, false, &error)) {
            // BC4 holds one channel: show it in R, G and B (and A = 1) like the greyscale source did.
            const VkComponentMapping swizzle = full.format == VK_FORMAT_BC4_UNORM_BLOCK
                                                   ? VkComponentMapping{VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R,
                                                                        VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ONE}
                                                   : VkComponentMapping{};
            tex = Texture::createWithLevels(*m_renderer, full.width, full.height, full.format, full.data.data(), full.data.size(),
                                            full.levelOffsets, settings.sampler(), swizzle, name);
            ++m_cookStats.texturesCooked;
        } else {
            if (state == CookedState::Current) log::warn("Cooked texture {}: {}", name, error);
            if (!source.empty()) {
                queueCook(name, [source, settings, cooked](const std::atomic<bool>& cancelled) {
                    const TextureCookResult r = cookTexture(source, settings, cooked, &cancelled);
                    char summary[128];
                    std::snprintf(summary, sizeof(summary), "%ux%u, %u mips, %.0f KB in %.0f ms", r.width, r.height, r.levels,
                                  double(r.bytes) / 1024.0, r.milliseconds);
                    return CookJobResult{r.ok, r.error, summary};
                });
            }
        }
    }
    if (!tex) {
        if (source.empty()) throw std::runtime_error("Asset not found: " + path.string() + " (no source image or cooked copy)");
        tex = loadTextureFile(*m_renderer, source, settings.srgb(), settings.mipmaps, settings.sampler());
        ++m_cookStats.texturesSource;
    }
    m_textures[key] = tex;
    return tex;
}

TextureSettings AssetManager::textureSettings(const std::filesystem::path& path)
{
    try {
        return TextureSettings::forImage(resolveCached(path));
    } catch (const std::exception&) {
        TextureSettings s;
        s.type = TextureSettings::typeFromName(path);
        // A packaged game: no source or .texture file, but the cooked copy
        // stores the settings it was made with.
        if (!m_cook.cacheDir.empty() && isCookableImage(path)) {
            const std::string name = path.is_absolute() ? assetName(path) : path.generic_string();
            Ktx2Texture header;
            if (readKtx2(cookedTexturePath(m_cook.cacheDir, name), header, true)) checkCookedTexture(header, {}, nullptr, &s);
        }
        return s;
    }
}

void AssetManager::setCooking(CookOptions options)
{
    m_cook = std::move(options);
}

void AssetManager::queueCook(const std::string& name, CookWork work)
{
    if (!m_cook.cookOnDemand || m_cook.synchronous || m_cookJobs.count(name)) return;
    // The job owns its inputs and result (Jobs.hpp rules): the manager can
    // drop it at any time, and it never touches engine state.
    auto result = std::make_shared<CookJobResult>();
    jobs::Handle h = jobs::submit([work = std::move(work), result](const std::atomic<bool>& cancelled) { *result = work(cancelled); },
                                  jobs::Lane::Background);
    m_cookJobs[name] = {h, result};
    m_cookStats.cooking = uint32_t(m_cookJobs.size());
}

void AssetManager::updateCooking()
{
    for (auto it = m_cookJobs.begin(); it != m_cookJobs.end();) {
        if (!it->second.handle.done()) {
            ++it;
            continue;
        }
        const CookJobResult& r = *it->second.result;
        if (r.ok) {
            ++m_cookStats.cookedNow;
            log::info("Cooked {}: {} (used from the next load)", it->first, r.summary);
        } else if (!it->second.handle.cancelled()) {
            ++m_cookStats.failed;
            log::warn("Cooking {} failed: {}", it->first, r.error);
        }
        it = m_cookJobs.erase(it);
    }
    m_cookStats.cooking = uint32_t(m_cookJobs.size());
}

AssetManager::~AssetManager()
{
    // Queued cooks never start; running ones finish on their own (they own their data).
    for (auto& [name, job] : m_cookJobs) job.handle.cancel();
}

std::shared_ptr<Model> AssetManager::loadModel(const std::filesystem::path& path)
{
    // Like loadTexture: the source if it's there (a packaged game may only
    // have the cooked copy), the cooked copy when it is current, else the
    // source - cooked on the way for next time.
    std::filesystem::path source;
    std::string name;
    try {
        source = resolveCached(path);
        name = assetName(source);
    } catch (const std::exception&) {
        if (m_cook.cacheDir.empty()) throw;
        name = path.is_absolute() ? assetName(path) : path.generic_string();
        if (std::filesystem::path(name).is_absolute()) throw;
    }
    const std::string key = source.empty() ? "cooked:" + name : source.string();
    if (auto it = m_models.find(key); it != m_models.end()) return it->second;
    const auto t0 = std::chrono::steady_clock::now();

    const ModelSettings settings = source.empty() ? ModelSettings{} : ModelSettings::forModel(source);
    // Texture URIs are relative to the model's folder: an asset path when the
    // model is in the asset folders (works with only cooked textures too).
    const std::filesystem::path modelPath = std::filesystem::path(name).is_absolute() ? source : std::filesystem::path(name);
    std::shared_ptr<Model> model;
    const char* how = "parsed";
    if (isCookableModel(path) && m_cook.useCooked && !m_cook.cacheDir.empty()) {
        const std::filesystem::path cooked = cookedModelPath(m_cook.cacheDir, name);
        ModelData data;
        std::string error;
        if (checkCookedModel(cooked, source, settings) == CookedState::Current && readCookedModel(cooked, nullptr, &data, &error)) {
            model = buildModel(*m_renderer, data, this, modelPath, settings);
            ++m_cookStats.modelsCooked;
            how = "cooked";
        } else if (!source.empty() && m_cook.cookOnDemand) {
            // Cook now: parse, build, generate LODs / impostors (they need the
            // renderer: this thread), use the result, and write the cooked copy
            // in the background (impostor compression is slow).
            std::vector<std::filesystem::path> deps = modelDependencies(source);
            auto d = std::make_shared<ModelData>(parseGltf(source));
            for (ModelMaterialData& m : d->materials)
                if (auto it = settings.materials.find(m.name); it != settings.materials.end()) m.file = it->second;
            std::vector<std::filesystem::path> materialFiles;
            model = buildModel(*m_renderer, *d, this, modelPath, settings, &materialFiles);
            if (settings.generatesLods()) {
                std::string genError;
                if (!modelLodGenerator()) log::warnOnce("Model " + name + ": LODs / impostors asked for, but no generator is registered");
                else if (modelLodGenerator()(*d, *model, settings, genError))
                    model->cookedLods = std::make_shared<const std::vector<ModelVariantLods>>(d->lods);
                else log::warn("Model {}: generating LODs / impostors failed: {}", name, genError);
            }
            for (const std::filesystem::path& f : materialFiles) {
                try {
                    deps.push_back(resolve(f));
                } catch (const std::exception&) {
                }
            }
            const nlohmann::json stamp = makeModelStamp(source, settings, deps);
            auto work = [d, stamp, cooked](const std::atomic<bool>& cancelled) {
                if (!compressModelImpostors(*d, &cancelled)) return CookJobResult{false, "cancelled", {}};
                std::string err;
                const bool ok = writeCookedModel(cooked, stamp, *d, &err);
                char summary[96];
                std::snprintf(summary, sizeof(summary), "%zu meshes, %zu LOD variants", d->meshes.size(), d->lods.size());
                return CookJobResult{ok, err, summary};
            };
            if (m_cook.synchronous) {
                const std::atomic<bool> never{false};
                const CookJobResult r = work(never);
                if (!r.ok) log::warn("Cooking {} failed: {}", name, r.error);
            } else {
                queueCook(name, std::move(work));
            }
            ++m_cookStats.modelsSource;
            how = "parsed, cooking";
        }
    }
    if (!model) {
        if (source.empty()) throw std::runtime_error("Asset not found: " + path.string() + " (no source model or cooked copy)");
        model = buildModel(*m_renderer, parseGltf(source), this, modelPath, settings);
        ++m_cookStats.modelsSource;
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    log::info("Loaded model {} ({} meshes, {} nodes; {}) in {:.1f} ms", path.string(), model->meshes.size(), model->nodes.size(), how, ms);
    m_models[key] = model;
    return model;
}

std::shared_ptr<AudioClip> AssetManager::loadAudio(const std::filesystem::path& path, bool stream)
{
    // Like loadTexture: the source if it's there, its cooked copy (Ogg Vorbis,
    // AudioCook.hpp) when that is current, else the source + a cook for next time.
    std::filesystem::path source;
    std::string name;
    try {
        source = resolveCached(path);
        name = assetName(source);
    } catch (const std::exception&) {
        // No source (a packaged game): its cooked copy, by asset name. An
        // absolute path (a glTF's image, next to the model) is named relative
        // to the search path it's under.
        if (m_cook.cacheDir.empty()) throw;
        name = path.is_absolute() ? assetName(path) : path.generic_string();
        if (std::filesystem::path(name).is_absolute()) throw; // (outside the asset folders)
    }
    const std::string key = (source.empty() ? "cooked:" + name : source.string()) + (stream ? "|stream" : "");
    if (auto it = m_audio.find(key); it != m_audio.end()) return it->second;

    std::filesystem::path file = source;
    if (isCookableAudio(path) && m_cook.useCooked && !m_cook.cacheDir.empty()) {
        AudioSettings settings = source.empty() ? AudioSettings{} : AudioSettings::forFile(source);
        if (settings.compress) {
            const std::filesystem::path cooked = cookedAudioPath(m_cook.cacheDir, name);
            CookedState state = checkCookedAudio(cooked, source, source.empty() ? nullptr : &settings);
            if (state != CookedState::Current && !source.empty() && m_cook.cookOnDemand && m_cook.synchronous) {
                const AudioCookResult r = cookAudio(source, settings, cooked);
                if (r.ok) state = CookedState::Current;
                else log::warn("Cooking {} failed: {}", name, r.error);
            }
            if (state == CookedState::Current) {
                file = cooked;
            } else if (!source.empty()) {
                queueCook(name, [source, settings, cooked](const std::atomic<bool>& cancelled) {
                    const AudioCookResult r = cookAudio(source, settings, cooked, &cancelled);
                    char summary[128];
                    std::snprintf(summary, sizeof(summary), "%.1f s, %u ch, %u Hz, %.0f KB in %.0f ms", r.seconds, r.channels,
                                  r.sampleRate, double(r.bytes) / 1024.0, r.milliseconds);
                    return CookJobResult{r.ok, r.error, summary};
                });
            }
        }
    }
    if (file.empty()) throw std::runtime_error("Asset not found: " + path.string() + " (no source file or cooked copy)");
    if (file == source) ++m_cookStats.soundsSource;
    else ++m_cookStats.soundsCooked;
    // The clip keeps the asset's own name: scene files store "audio/fire.wav" either way.
    auto clip = std::make_shared<AudioClip>(file.string(), stream, name);
    m_audio[key] = clip;
    return clip;
}

// -----------------------------------------------------------------------------
// Material files (MaterialAsset.hpp)
// -----------------------------------------------------------------------------

std::shared_ptr<StandardMaterial> AssetManager::loadMaterial(const std::filesystem::path& path)
{
    const std::filesystem::path& file = resolveCached(path);
    // Case-insensitive key: Windows and .vepak archives find "Thatch.material"
    // as thatch.material, which must stay one shared Material.
    std::string key = file.generic_string();
    for (char& c : key) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (auto it = m_materials.find(key); it != m_materials.end()) return it->second.material;
    LoadedMaterial e;
    e.asset = assetName(file); // (DataAssets finds the source copy in a development build: live edits)
    e.material = m_renderer->createStandardMaterial();
    e.material->name = file.stem().string();
    e.applied = DataAssets::get(e.asset);
    MaterialAsset::fromJson(*e.applied).apply(*e.material, *this);
    m_materialGeneration = DataAssets::generation();
    return m_materials.emplace(key, std::move(e)).first->second.material;
}

std::string AssetManager::materialFileForGltf(const std::string& name, const std::string& extrasJson) const
{
    // 1. An explicit "material" custom property.
    if (!extrasJson.empty()) {
        const nlohmann::json extras = nlohmann::json::parse(extrasJson, nullptr, false);
        if (extras.is_object() && extras.contains("material") && extras["material"].is_string()) return extras["material"].get<std::string>();
    }
    // 2. A file named after it. Blender names copies "Concrete.001": those
    // use Concrete's file too. File systems differ in case: lower case first
    // (the usual file name, and what Windows / archives match for "Concrete").
    if (materialFolder.empty() || name.empty()) return {};
    std::string base = name;
    if (const size_t dot = base.rfind('.'); dot != std::string::npos && dot + 1 < base.size() &&
        std::all_of(base.begin() + long(dot) + 1, base.end(), [](unsigned char c) { return std::isdigit(c); }))
        base.resize(dot);
    std::string lower = base;
    for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
    for (const std::string& candidate : {lower, base}) {
        const std::filesystem::path p = std::filesystem::path(materialFolder) / (candidate + ".material");
        if (exists(p)) return p.generic_string();
    }
    return {};
}

std::shared_ptr<StandardMaterial> AssetManager::materialForGltf(const std::string& name, const std::string& extrasJson)
{
    const std::string file = materialFileForGltf(name, extrasJson);
    if (file.empty()) return nullptr;
    try {
        auto material = loadMaterial(file);
        log::info("glTF material '{}' uses {}", name, file); // (a file overrides by name: say so)
        return material;
    } catch (const std::exception& e) {
        log::warn("glTF material '{}': {}", name, e.what());
        return nullptr;
    }
}

std::shared_ptr<Shader> AssetManager::materialShader(StandardMaterial::Mode mode, const std::string& vertex,
                                                     const std::string& fragment)
{
    const BuiltinShader builtin = mode == StandardMaterial::Mode::Cutout        ? BuiltinShader::StandardCutout
                                  : mode == StandardMaterial::Mode::Transparent ? BuiltinShader::StandardTransparent
                                                                                : BuiltinShader::Standard;
    std::shared_ptr<Shader> base = m_renderer->builtinShader(builtin);
    if (vertex.empty() && fragment.empty()) return base;
    const std::string key = std::to_string(int(mode)) + "|" + vertex + "|" + fragment;
    if (auto it = m_materialShaders.find(key); it != m_materialShaders.end()) return it->second;
    // A missing program would only fail when the pipeline is built mid-frame:
    // check now and fall back to the standard shader.
    try {
        if (!vertex.empty()) m_renderer->findShader(vertex);
        if (!fragment.empty()) m_renderer->findShader(fragment);
    } catch (const std::exception& e) {
        log::warn("Material shader: {} (using the standard shader)", e.what());
        return base;
    }
    ShaderDesc d = base->desc(); // the mode's state (blending, culling, cutout shadows...)
    d.name = "Material " + (fragment.empty() ? vertex : fragment);
    if (!vertex.empty()) d.vertex = vertex;
    if (!fragment.empty()) d.fragment = fragment;
    auto shader = m_renderer->createShader(d);
    m_materialShaders[key] = shader;
    return shader;
}

void AssetManager::updateMaterials()
{
    if (m_materials.empty()) return;
    const uint64_t generation = DataAssets::generation();
    if (generation == m_materialGeneration) return;
    m_materialGeneration = generation;
    for (auto& [key, e] : m_materials) {
        std::shared_ptr<const nlohmann::json> content = DataAssets::get(e.asset);
        if (content == e.applied) continue; // (another data file changed)
        e.applied = content;
        MaterialAsset::fromJson(*content).apply(*e.material, *this);
        log::info("Material {} updated", e.asset);
    }
}

// -----------------------------------------------------------------------------
// Texture loading (stb_image)
// -----------------------------------------------------------------------------

std::shared_ptr<Texture> loadTextureFile(Renderer& renderer, const std::filesystem::path& file, bool srgb,
                                         bool mipmaps, SamplerType sampler)
{
    const std::string path = file.string();
    const auto bytes = vfs::read(file);
    if (!bytes) throw std::runtime_error("Can't read " + path);
    const auto* data = bytes->data();
    const int size = static_cast<int>(bytes->size());
    int w = 0, h = 0, channels = 0;
    if (stbi_is_hdr_from_memory(data, size)) {
        // HDR images stay floating point; float32 blits aren't guaranteed to
        // support linear filtering, so no mips.
        float* px = stbi_loadf_from_memory(data, size, &w, &h, &channels, 4);
        if (!px) throw std::runtime_error("Failed to load " + path + ": " + stbi_failure_reason());
        auto tex = Texture::create2D(renderer, uint32_t(w), uint32_t(h), VK_FORMAT_R32G32B32A32_SFLOAT, px,
                                     size_t(w) * h * 16, false, sampler, file.filename().string());
        stbi_image_free(px);
        return tex;
    }
    stbi_uc* px = stbi_load_from_memory(data, size, &w, &h, &channels, 4); // always expand to RGBA
    if (!px) throw std::runtime_error("Failed to load " + path + ": " + stbi_failure_reason());
    auto tex = Texture::create2D(renderer, uint32_t(w), uint32_t(h),
                                 srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, px, size_t(w) * h * 4,
                                 mipmaps, sampler, file.filename().string());
    stbi_image_free(px);
    return tex;
}

ImagePixels loadImagePixels(const std::filesystem::path& file)
{
    const std::string path = file.string();
    const auto bytes = vfs::read(file);
    if (!bytes) throw std::runtime_error("Can't read " + path);
    int w = 0, h = 0, channels = 0;
    stbi_uc* px = stbi_load_from_memory(bytes->data(), static_cast<int>(bytes->size()), &w, &h, &channels, 4);
    if (!px) throw std::runtime_error("Failed to load " + path + ": " + stbi_failure_reason());
    ImagePixels img{w, h, std::vector<uint8_t>(px, px + size_t(w) * h * 4)};
    stbi_image_free(px);
    return img;
}

std::shared_ptr<Texture> loadTextureMemory(Renderer& renderer, const void* data, size_t size, bool srgb,
                                           bool mipmaps, SamplerType sampler, const std::string& name)
{
    int w = 0, h = 0, channels = 0;
    stbi_uc* px = stbi_load_from_memory(static_cast<const stbi_uc*>(data), static_cast<int>(size), &w, &h,
                                        &channels, 4);
    if (!px) throw std::runtime_error("Failed to decode image " + name + ": " + stbi_failure_reason());
    auto tex = Texture::create2D(renderer, uint32_t(w), uint32_t(h),
                                 srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, px, size_t(w) * h * 4,
                                 mipmaps, sampler, name);
    stbi_image_free(px);
    return tex;
}

} // namespace ve
