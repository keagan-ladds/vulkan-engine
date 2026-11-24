#pragma once

// =============================================================================
// AssetManager - loads files from disk once and caches them by path.
//
//   auto tex   = assets().loadTexture("textures/rock.png");     // colour, data or normal: decided by the file
//   auto nrm   = assets().loadTexture("textures/rock_n.png");   // (TextureSettings.hpp: .texture file or name)
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
//
// Cooked textures (TextureCook.hpp): loadTexture uses the GPU-ready copy in
// the cook cache (<exe dir>/cooked by default) when it is current; otherwise
// it loads the source image and, with cooking on, cooks it on a worker thread
// for the next run. A shipped build can contain only the cooked files.
// Env: VE_COOK=0 (always load the source images), VE_COOK=read (use cooked
// files, never cook), VE_COOK=sync (cook right away, before loading),
// VE_COOK_DIR=<folder> (another cache).
// =============================================================================

#include "ve/assets/TextureSettings.hpp"
#include "ve/core/Jobs.hpp"
#include "ve/gfx/Samplers.hpp"
#include "ve/render/Material.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
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

/// 8-bit RGBA pixels of an image file (for building textures on the CPU).
struct ImagePixels {
    int width = 0;             ///< Pixels across.
    int height = 0;            ///< Pixels down.
    std::vector<uint8_t> rgba; ///< width * height * 4 bytes, rows top to bottom.
};

/// Loads and caches assets by path: textures, models, sounds and material
/// files, found through the search paths (`<exe>/assets` and the game's) and
/// mounted archives, using cooked copies when current (Engine::assets()).
/// Loads are cached: the same path gives the same shared object. Main thread only.
class AssetManager {
public:
    AssetManager() = default;
    ~AssetManager();
    AssetManager(const AssetManager&) = delete;
    AssetManager& operator=(const AssetManager&) = delete;

    /// Engine: sets the renderer and the folders assets are looked up in, in order.
    void init(Renderer& renderer, std::vector<std::filesystem::path> searchPaths);
    void clear(); ///< drops the cache (assets still referenced elsewhere stay alive)

    /// Adds a folder to look assets up in (after the existing ones).
    void addSearchPath(const std::filesystem::path& path);
    /// The folders assets are looked up in, first match wins.
    const std::vector<std::filesystem::path>& searchPaths() const { return m_searchPaths; }
    /// Absolute path of an asset (first search path that has it); throws
    /// std::runtime_error if it can't be found.
    std::filesystem::path resolve(const std::filesystem::path& path) const;
    /// Whether an asset can be loaded: the file exists, or - for a cookable
    /// file (image, sound) - its cooked copy does, so a packaged game without
    /// the sources still finds its images and sounds.
    bool exists(const std::filesystem::path& path) const;
    /// How scene files refer to a file: relative to the search path that holds
    /// it ("audio/fire.wav"), or the path unchanged if it's outside them.
    std::string assetName(const std::filesystem::path& file) const;

    /// An image's 8-bit RGBA pixels on the CPU (e.g. to combine maps): the
    /// source image, or - in a packaged game, where only the cooked copy ships -
    /// the cooked copy's full-size level decoded (TextureCook.hpp).
    ImagePixels loadPixels(const std::filesystem::path& path) const;
    /// PNG/JPG/TGA/BMP/PSD/GIF (8-bit) or HDR (float). How it's loaded (colour,
    /// data or normal map, mips, sampler...) is the file's TextureSettings:
    /// its .texture file, else its name. One texture per file.
    std::shared_ptr<Texture> loadTexture(const std::filesystem::path& path);
    /// The settings loadTexture uses for an image (defaults if it can't be found).
    TextureSettings textureSettings(const std::filesystem::path& path);
    /// glTF 2.0 (.gltf with external/embedded buffers, or .glb).
    std::shared_ptr<Model> loadModel(const std::filesystem::path& path);
    /// WAV/MP3/FLAC/OGG. stream = decode while playing (long music) instead of up front.
    std::shared_ptr<AudioClip> loadAudio(const std::filesystem::path& path, bool stream = false);
    /// A *.material file (MaterialAsset.hpp): one shared StandardMaterial per
    /// file, updated in place when the file changes.
    std::shared_ptr<StandardMaterial> loadMaterial(const std::filesystem::path& path);

    /// glTF materials named X use "<materialFolder>/X.material" (or x.material)
    /// when that file exists. Empty = only materials with a "material" custom
    /// property use files. Set before loading models.
    std::string materialFolder = "materials";

    // --- Engine internal -------------------------------------------------------------
    /// The material file for a glTF material (its "material" extras property,
    /// else by name; see materialFolder), or null. Logs a missing named file.
    std::shared_ptr<StandardMaterial> materialForGltf(const std::string& name, const std::string& extrasJson);
    /// The shader for a material file: built-in for its mode, or a custom
    /// vertex / fragment pair with that mode's state (shared per combination).
    std::shared_ptr<Shader> materialShader(StandardMaterial::Mode mode, const std::string& vertex, const std::string& fragment);
    /// Re-applies material files that changed (DataAssets reloads, editor edits). Every frame.
    void updateMaterials();

    // --- Cooking -------------------------------------------------------------------
    /// How the cook cache is used (EngineConfig::cookCache / cookOnDemand, env VE_COOK).
    struct CookOptions {
        std::filesystem::path cacheDir; ///< empty = no cooked files at all
        bool useCooked = true;          ///< load current cooked copies
        bool cookOnDemand = true;       ///< cook missing / stale ones (worker thread)
        bool synchronous = false;       ///< ...right away instead (tests, tools)
    };
    /// Engine: sets how cooked files are used (before anything loads).
    void setCooking(CookOptions options);
    /// The cooking options in effect (cooking().cacheDir: where cooked files live).
    const CookOptions& cooking() const { return m_cook; }
    /// Counters for the F1 overlay and tests: where loaded assets came from.
    struct CookStats {
        uint32_t texturesCooked = 0; ///< textures loaded from cooked files
        uint32_t texturesSource = 0; ///< textures loaded from source images
        uint32_t soundsCooked = 0;   ///< sounds playing cooked (Ogg Vorbis) files
        uint32_t soundsSource = 0;   ///< sounds playing their source files
        uint32_t cooking = 0;        ///< cook jobs running or queued
        uint32_t cookedNow = 0;      ///< cooks finished this run
        uint32_t failed = 0;         ///< cooks that failed this run (logged)
    };
    /// Where loaded textures and sounds came from, and cooking progress.
    const CookStats& cookStats() const { return m_cookStats; }
    /// Collects finished cook jobs (logs them). Every frame.
    void updateCooking();

private:
    const std::filesystem::path& resolveCached(const std::filesystem::path& path); ///< resolve(), remembered
    Renderer* m_renderer = nullptr;
    std::unordered_map<std::string, std::filesystem::path> m_resolved; ///< requested -> resolved
    std::vector<std::filesystem::path> m_searchPaths;
    std::unordered_map<std::string, std::shared_ptr<Texture>> m_textures;
    std::unordered_map<std::string, std::shared_ptr<Model>> m_models;
    std::unordered_map<std::string, std::shared_ptr<AudioClip>> m_audio;
    struct LoadedMaterial {
        std::string asset; ///< its DataAssets path
        std::shared_ptr<StandardMaterial> material;
        std::shared_ptr<const nlohmann::json> applied; ///< the content last applied
    };
    std::unordered_map<std::string, LoadedMaterial> m_materials; ///< by resolved path
    std::unordered_map<std::string, std::shared_ptr<Shader>> m_materialShaders;
    uint64_t m_materialGeneration = 0;

    /// What a cook job reports back (to the main thread, updateCooking()).
    struct CookJobResult {
        bool ok = false;
        std::string error;
        std::string summary; ///< logged when it succeeded
    };
    using CookWork = std::function<CookJobResult(const std::atomic<bool>& cancelled)>;
    /// Runs `work` on the job pool (Background lane) unless cooking on demand
    /// is off, or a cook of `name` is already running.
    void queueCook(const std::string& name, CookWork work);
    CookOptions m_cook;
    CookStats m_cookStats;
    struct CookJob {
        jobs::Handle handle;
        std::shared_ptr<CookJobResult> result;
    };
    std::unordered_map<std::string, CookJob> m_cookJobs; ///< by asset name
    bool m_warnedNoBC = false;
};

ImagePixels loadImagePixels(const std::filesystem::path& file);

/// Lower-level loaders (used by AssetManager; usable directly).
std::shared_ptr<Texture> loadTextureFile(Renderer& renderer, const std::filesystem::path& file, bool srgb,
                                         bool mipmaps, SamplerType sampler);
std::shared_ptr<Texture> loadTextureMemory(Renderer& renderer, const void* data, size_t size, bool srgb,
                                           bool mipmaps, SamplerType sampler, const std::string& name);
/// With `assets`: image files go through its texture cache (shared between
/// models) and material files replace glTF materials (see materialFolder).
std::shared_ptr<Model> loadGltf(Renderer& renderer, const std::filesystem::path& file, AssetManager* assets = nullptr);

} // namespace ve
