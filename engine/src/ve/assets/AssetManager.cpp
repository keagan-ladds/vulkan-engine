#include "ve/assets/AssetManager.hpp"

#include "ve/assets/DataAsset.hpp"
#include "ve/assets/MaterialAsset.hpp"
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

std::shared_ptr<Texture> AssetManager::loadTexture(const std::filesystem::path& path, bool srgb, bool mipmaps,
                                                   SamplerType sampler)
{
    const std::filesystem::path& file = resolveCached(path);
    // Everything that changes the texture is in the key: the same image as
    // colour and as data, with or without mips, or with another sampler, are
    // different textures.
    const std::string key = file.string() + (srgb ? "|srgb" : "|linear") + (mipmaps ? "|mips" : "|nomips") + "|sampler" +
                            std::to_string(static_cast<int>(sampler));
    if (auto it = m_textures.find(key); it != m_textures.end()) return it->second;
    auto tex = loadTextureFile(*m_renderer, file, srgb, mipmaps, sampler);
    m_textures[key] = tex;
    return tex;
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
