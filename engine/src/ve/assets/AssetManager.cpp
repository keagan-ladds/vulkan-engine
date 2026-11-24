#include "ve/assets/AssetManager.hpp"

#include "ve/assets/DataAsset.hpp"
#include "ve/assets/MaterialAsset.hpp"
#include "ve/assets/TextureCook.hpp"
#include "ve/gfx/Device.hpp"
#include "ve/audio/Audio.hpp"
#include "ve/assets/Model.hpp"
#include "ve/core/Log.hpp"
#include "ve/render/Renderer.hpp"
#include "ve/render/Texture.hpp"

#include <stb_image.h>

#include <algorithm>
#include <cctype>
#include <chrono>

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

bool AssetManager::exists(const std::filesystem::path& path) const
{
    if (path.is_absolute()) return std::filesystem::exists(path);
    for (const auto& root : m_searchPaths) {
        if (std::filesystem::exists(root / path)) return true;
    }
    return false;
}

std::filesystem::path AssetManager::resolve(const std::filesystem::path& path) const
{
    if (path.is_absolute()) {
        if (std::filesystem::exists(path)) return path;
    } else {
        for (const auto& root : m_searchPaths) {
            auto p = root / path;
            if (std::filesystem::exists(p)) return std::filesystem::canonical(p);
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
        if (path.is_absolute() || m_cook.cacheDir.empty()) throw;
        name = path.generic_string();
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
            ++m_texStats.cooked;
        } else {
            if (state == CookedState::Current) log::warn("Cooked texture {}: {}", name, error);
            if (!source.empty()) queueCook(source, name, settings);
        }
    }
    if (!tex) {
        if (source.empty()) throw std::runtime_error("Asset not found: " + path.string() + " (no source image or cooked copy)");
        tex = loadTextureFile(*m_renderer, source, settings.srgb(), settings.mipmaps, settings.sampler());
        ++m_texStats.source;
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
        return s;
    }
}

void AssetManager::setCooking(CookOptions options)
{
    m_cook = std::move(options);
}

void AssetManager::queueCook(const std::filesystem::path& source, const std::string& name, const TextureSettings& settings)
{
    if (!m_cook.cookOnDemand || m_cook.synchronous || m_cookJobs.count(name)) return;
    // The job owns its inputs and result (Jobs.hpp rules): the manager can
    // drop it at any time, and it never touches engine state.
    auto result = std::make_shared<TextureCookResult>();
    const std::filesystem::path output = cookedTexturePath(m_cook.cacheDir, name);
    jobs::Handle h = jobs::submit(
        [source, settings, output, result](const std::atomic<bool>& cancelled) { *result = cookTexture(source, settings, output, &cancelled); },
        jobs::Lane::Background);
    m_cookJobs[name] = {name, h, result};
    m_texStats.cooking = uint32_t(m_cookJobs.size());
}

void AssetManager::updateCooking()
{
    for (auto it = m_cookJobs.begin(); it != m_cookJobs.end();) {
        if (!it->second.handle.done()) {
            ++it;
            continue;
        }
        const TextureCookResult& r = *it->second.result;
        if (r.ok) {
            ++m_texStats.cookedNow;
            log::info("Cooked {}: {}x{}, {} mips, {:.0f} KB in {:.0f} ms (used from the next load)", it->first, r.width, r.height,
                      r.levels, double(r.bytes) / 1024.0, r.milliseconds);
        } else if (!it->second.handle.cancelled()) {
            ++m_texStats.failed;
            log::warn("Cooking {} failed: {}", it->first, r.error);
        }
        it = m_cookJobs.erase(it);
    }
    m_texStats.cooking = uint32_t(m_cookJobs.size());
}

AssetManager::~AssetManager()
{
    // Queued cooks never start; running ones finish on their own (they own their data).
    for (auto& [name, job] : m_cookJobs) job.handle.cancel();
}

std::shared_ptr<Model> AssetManager::loadModel(const std::filesystem::path& path)
{
    const std::filesystem::path& file = resolveCached(path);
    const std::string key = file.string();
    if (auto it = m_models.find(key); it != m_models.end()) return it->second;
    const auto t0 = std::chrono::steady_clock::now();
    auto model = loadGltf(*m_renderer, file, this);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    log::info("Loaded model {} ({} meshes, {} nodes) in {:.1f} ms", path.string(), model->meshes.size(),
              model->nodes.size(), ms);
    m_models[key] = model;
    return model;
}

std::shared_ptr<AudioClip> AssetManager::loadAudio(const std::filesystem::path& path, bool stream)
{
    const std::filesystem::path& file = resolveCached(path);
    const std::string key = file.string() + (stream ? "|stream" : "");
    if (auto it = m_audio.find(key); it != m_audio.end()) return it->second;
    auto clip = std::make_shared<AudioClip>(file.string(), stream, assetName(file));
    m_audio[key] = clip;
    return clip;
}

// -----------------------------------------------------------------------------
// Material files (MaterialAsset.hpp)
// -----------------------------------------------------------------------------

std::shared_ptr<StandardMaterial> AssetManager::loadMaterial(const std::filesystem::path& path)
{
    const std::filesystem::path& file = resolveCached(path);
    const std::string key = file.string();
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

std::shared_ptr<StandardMaterial> AssetManager::materialForGltf(const std::string& name, const std::string& extrasJson)
{
    // 1. An explicit "material" custom property.
    if (!extrasJson.empty()) {
        const nlohmann::json extras = nlohmann::json::parse(extrasJson, nullptr, false);
        if (extras.is_object() && extras.contains("material") && extras["material"].is_string()) {
            const std::string path = extras["material"].get<std::string>();
            try {
                return loadMaterial(path);
            } catch (const std::exception& e) {
                log::warn("glTF material '{}': {}", name, e.what());
                return nullptr;
            }
        }
    }
    // 2. A file named after it. Blender names copies "Concrete.001": those
    // use Concrete's file too. File systems differ in case: also try lower case.
    if (materialFolder.empty() || name.empty()) return nullptr;
    std::string base = name;
    if (const size_t dot = base.rfind('.'); dot != std::string::npos && dot + 1 < base.size() &&
        std::all_of(base.begin() + long(dot) + 1, base.end(), [](unsigned char c) { return std::isdigit(c); }))
        base.resize(dot);
    std::string lower = base;
    for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
    for (const std::string& candidate : {base, lower}) {
        const std::filesystem::path p = std::filesystem::path(materialFolder) / (candidate + ".material");
        if (!exists(p)) continue;
        try {
            auto material = loadMaterial(p);
            log::info("glTF material '{}' uses {}", name, p.generic_string()); // (a file overrides by name: say so)
            return material;
        } catch (const std::exception& e) {
            log::warn("glTF material '{}': {}", name, e.what());
            return nullptr;
        }
    }
    return nullptr;
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
    int w = 0, h = 0, channels = 0;
    if (stbi_is_hdr(path.c_str())) {
        // HDR images stay floating point; float32 blits aren't guaranteed to
        // support linear filtering, so no mips.
        float* px = stbi_loadf(path.c_str(), &w, &h, &channels, 4);
        if (!px) throw std::runtime_error("Failed to load " + path + ": " + stbi_failure_reason());
        auto tex = Texture::create2D(renderer, uint32_t(w), uint32_t(h), VK_FORMAT_R32G32B32A32_SFLOAT, px,
                                     size_t(w) * h * 16, false, sampler, file.filename().string());
        stbi_image_free(px);
        return tex;
    }
    stbi_uc* px = stbi_load(path.c_str(), &w, &h, &channels, 4); // always expand to RGBA
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
    int w = 0, h = 0, channels = 0;
    stbi_uc* px = stbi_load(path.c_str(), &w, &h, &channels, 4);
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
