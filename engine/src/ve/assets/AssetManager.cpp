#include "ve/assets/AssetManager.hpp"

#include "ve/audio/Audio.hpp"
#include "ve/assets/Model.hpp"
#include "ve/core/Log.hpp"
#include "ve/render/Renderer.hpp"
#include "ve/render/Texture.hpp"

#include <stb_image.h>

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
}

void AssetManager::addSearchPath(const std::filesystem::path& path)
{
    m_searchPaths.push_back(path);
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
    const auto file = resolve(path);
    const std::string key = file.string() + (srgb ? "|srgb" : "|linear");
    if (auto it = m_textures.find(key); it != m_textures.end()) return it->second;
    auto tex = loadTextureFile(*m_renderer, file, srgb, mipmaps, sampler);
    m_textures[key] = tex;
    return tex;
}

std::shared_ptr<Model> AssetManager::loadModel(const std::filesystem::path& path)
{
    const auto file = resolve(path);
    const std::string key = file.string();
    if (auto it = m_models.find(key); it != m_models.end()) return it->second;
    const auto t0 = std::chrono::steady_clock::now();
    auto model = loadGltf(*m_renderer, file);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    log::info("Loaded model {} ({} meshes, {} nodes) in {:.1f} ms", path.string(), model->meshes.size(),
              model->nodes.size(), ms);
    m_models[key] = model;
    return model;
}

std::shared_ptr<AudioClip> AssetManager::loadAudio(const std::filesystem::path& path, bool stream)
{
    const auto file = resolve(path);
    const std::string key = file.string() + (stream ? "|stream" : "");
    if (auto it = m_audio.find(key); it != m_audio.end()) return it->second;
    auto clip = std::make_shared<AudioClip>(file.string(), stream, assetName(file));
    m_audio[key] = clip;
    return clip;
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
