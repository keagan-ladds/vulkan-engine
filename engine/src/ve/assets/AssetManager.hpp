#pragma once

// =============================================================================
// AssetManager - loads files from disk once and caches them by path.
//
//   auto tex   = assets().loadTexture("textures/rock.png");          // sRGB colour
//   auto nrm   = assets().loadTexture("textures/rock_n.png", false); // linear data
//   auto model = assets().loadModel("models/hut.gltf");
//   auto clip  = assets().loadAudio("audio/wind.wav");
//
// Relative paths are searched in each search path in order (by default
// <exe dir>/assets). Absolute paths are used as-is.
// =============================================================================

#include "ve/gfx/Samplers.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ve {

class Renderer;
class Texture;
class Model;
class AudioClip;

class AssetManager {
public:
    void init(Renderer& renderer, std::vector<std::filesystem::path> searchPaths);
    void clear(); // drops the cache (assets still referenced elsewhere stay alive)

    void addSearchPath(const std::filesystem::path& path);
    const std::vector<std::filesystem::path>& searchPaths() const { return m_searchPaths; }
    // Absolute path of an asset; throws if it can't be found.
    std::filesystem::path resolve(const std::filesystem::path& path) const;
    bool exists(const std::filesystem::path& path) const;
    // How scene files refer to a file: relative to the search path that holds
    // it ("audio/fire.wav"), or the path unchanged if it's outside them.
    std::string assetName(const std::filesystem::path& file) const;

    // PNG/JPG/TGA/BMP/PSD/GIF (8-bit) or HDR (float). `srgb` = colour data;
    // use false for normal maps, roughness, masks, etc.
    std::shared_ptr<Texture> loadTexture(const std::filesystem::path& path, bool srgb = true, bool mipmaps = true,
                                         SamplerType sampler = SamplerType::LinearRepeat);
    // glTF 2.0 (.gltf with external/embedded buffers, or .glb).
    std::shared_ptr<Model> loadModel(const std::filesystem::path& path);
    // WAV/MP3/FLAC/OGG. stream = decode while playing (long music) instead of up front.
    std::shared_ptr<AudioClip> loadAudio(const std::filesystem::path& path, bool stream = false);

private:
    Renderer* m_renderer = nullptr;
    std::vector<std::filesystem::path> m_searchPaths;
    std::unordered_map<std::string, std::shared_ptr<Texture>> m_textures;
    std::unordered_map<std::string, std::shared_ptr<Model>> m_models;
    std::unordered_map<std::string, std::shared_ptr<AudioClip>> m_audio;
};

// Lower-level loaders (used by AssetManager; usable directly).
std::shared_ptr<Texture> loadTextureFile(Renderer& renderer, const std::filesystem::path& file, bool srgb,
                                         bool mipmaps, SamplerType sampler);
std::shared_ptr<Texture> loadTextureMemory(Renderer& renderer, const void* data, size_t size, bool srgb,
                                           bool mipmaps, SamplerType sampler, const std::string& name);
std::shared_ptr<Model> loadGltf(Renderer& renderer, const std::filesystem::path& file);

} // namespace ve
