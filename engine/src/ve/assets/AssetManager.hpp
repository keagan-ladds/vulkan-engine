#pragma once

// =============================================================================
// AssetManager - loads files from disk once and caches them by path.
//
//   auto tex   = assets().loadTexture("textures/rock.png");          // sRGB colour
//   auto nrm   = assets().loadTexture("textures/rock_n.png", false); // linear data
//   auto model = assets().loadModel("models/hut.gltf");
//   auto clip  = assets().loadAudio("audio/wind.wav");
//   auto mat   = assets().loadMaterial("materials/concrete.material"); // shared (MaterialAsset.hpp)
//
// Textures are shared by path: models whose glTF files point at the same
// image file (exported as "glTF Separate" into one folder tree) get one GPU
// texture between them. Images embedded in a .glb stay per model.
//
// Relative paths are searched in each search path in order (by default
// <exe dir>/assets). Absolute paths are used as-is.
// =============================================================================

#include "ve/gfx/Samplers.hpp"
#include "ve/render/Material.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
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
class Material;
class Shader;

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
    // A *.material file (MaterialAsset.hpp): one shared StandardMaterial per
    // file, updated in place when the file changes.
    std::shared_ptr<StandardMaterial> loadMaterial(const std::filesystem::path& path);

    // glTF materials named X use "<materialFolder>/X.material" (or x.material)
    // when that file exists. Empty = only materials with a "material" custom
    // property use files. Set before loading models.
    std::string materialFolder = "materials";

    // --- Engine internal -------------------------------------------------------------
    // The material file for a glTF material (its "material" extras property,
    // else by name; see materialFolder), or null. Logs a missing named file.
    std::shared_ptr<StandardMaterial> materialForGltf(const std::string& name, const std::string& extrasJson);
    // The shader for a material file: built-in for its mode, or a custom
    // vertex / fragment pair with that mode's state (shared per combination).
    std::shared_ptr<Shader> materialShader(StandardMaterial::Mode mode, const std::string& vertex, const std::string& fragment);
    // Re-applies material files that changed (DataAssets reloads, editor edits). Every frame.
    void updateMaterials();

private:
    const std::filesystem::path& resolveCached(const std::filesystem::path& path); // resolve(), remembered
    Renderer* m_renderer = nullptr;
    std::unordered_map<std::string, std::filesystem::path> m_resolved; // requested -> resolved
    std::vector<std::filesystem::path> m_searchPaths;
    std::unordered_map<std::string, std::shared_ptr<Texture>> m_textures;
    std::unordered_map<std::string, std::shared_ptr<Model>> m_models;
    std::unordered_map<std::string, std::shared_ptr<AudioClip>> m_audio;
    struct LoadedMaterial {
        std::string asset; // its DataAssets path
        std::shared_ptr<StandardMaterial> material;
        std::shared_ptr<const nlohmann::json> applied; // the content last applied
    };
    std::unordered_map<std::string, LoadedMaterial> m_materials; // by resolved path
    std::unordered_map<std::string, std::shared_ptr<Shader>> m_materialShaders;
    uint64_t m_materialGeneration = 0;
};

// 8-bit RGBA pixels of an image file (for building textures on the CPU).
struct ImagePixels {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
};
ImagePixels loadImagePixels(const std::filesystem::path& file);

// Lower-level loaders (used by AssetManager; usable directly).
std::shared_ptr<Texture> loadTextureFile(Renderer& renderer, const std::filesystem::path& file, bool srgb,
                                         bool mipmaps, SamplerType sampler);
std::shared_ptr<Texture> loadTextureMemory(Renderer& renderer, const void* data, size_t size, bool srgb,
                                           bool mipmaps, SamplerType sampler, const std::string& name);
// With `assets`: image files go through its texture cache (shared between
// models) and material files replace glTF materials (see materialFolder).
std::shared_ptr<Model> loadGltf(Renderer& renderer, const std::filesystem::path& file, AssetManager* assets = nullptr);

} // namespace ve
