#pragma once

// =============================================================================
// ModelCook - glTF models cooked to `<cache>/<asset path>.vemodel`: the model
// as ModelData (ModelData.hpp) in binary, plus a stamp (cooker version,
// settings, the source's and its dependencies' size / time / hash) so a
// stale copy is recooked. Loading a cooked model reads arrays and uploads
// them: no glTF parsing, normals / tangents already computed.
//
// With a `<model>.model` file asking for them (ModelSettings), cooking also
// generates LOD levels and bakes impostors. That part needs the renderer
// (impostors are rendered), so it is registered by the world module
// (setModelLodGenerator) and runs on the main thread: in the game on demand
// (the first load of a stale model), or in `vecook` on its own headless
// renderer. Impostor atlases are block-compressed (BC7) before writing.
// =============================================================================

#include "ve/assets/Cook.hpp"
#include "ve/assets/ModelData.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace ve {

class Model;

/// Bump when the cooked model format or what cooking computes changes: every model is recooked.
constexpr uint32_t kModelCookerVersion = 1;

/// Model files the cooker handles (.gltf, .glb).
bool isCookableModel(const std::filesystem::path& file);
/// Where the cooked copy of a model asset lives: <cacheDir>/<asset path>.vemodel.
std::filesystem::path cookedModelPath(const std::filesystem::path& cacheDir, const std::string& assetPath);
/// Files a model is made from besides itself (external .bin buffers), as paths
/// next to it. Reads only the glTF's JSON. Empty for .glb files and on errors.
std::vector<std::filesystem::path> modelDependencies(const std::filesystem::path& source);

/// The stamp of a model cooked now from `source` with `settings`, made from
/// `dependencies` too (buffers, material files...: any file whose change should
/// recook it). Take it before reading the files.
nlohmann::json makeModelStamp(const std::filesystem::path& source, const ModelSettings& settings,
                              const std::vector<std::filesystem::path>& dependencies);
/// Checks a cooked model against its source, settings and dependencies (header
/// only). A missing source (packaged game) only checks the cooker version.
CookedState checkCookedModel(const std::filesystem::path& cooked, const std::filesystem::path& source, const ModelSettings& settings);

/// Writes a cooked model (via a temporary file). False (and `error`) on failure.
bool writeCookedModel(const std::filesystem::path& file, const nlohmann::json& stamp, const ModelData& data, std::string* error = nullptr);
/// Reads a cooked model: its stamp, and (if `data` isn't null) the model.
/// False (and `error`) if it's missing or unreadable.
bool readCookedModel(const std::filesystem::path& file, nlohmann::json* stamp, ModelData* data, std::string* error = nullptr);

/// Generates LOD levels and impostors into `data.lods` as `settings` ask,
/// from the built `model` (its GPU meshes and materials). Main thread (renders
/// impostors); leaves impostor atlases uncompressed (compressModelImpostors).
/// Registered by the world module (it owns LOD extraction and impostors).
/// @return false (and `error`) on failure.
using ModelLodGenerator = std::function<bool(ModelData& data, const Model& model, const ModelSettings& settings, std::string& error)>;
/// Registers the LOD generator (world module, at startup).
void setModelLodGenerator(ModelLodGenerator generator);
/// The registered generator (null if the world module didn't register one).
const ModelLodGenerator& modelLodGenerator();

/// Block-compresses uncompressed impostor atlases in `data` (BC7, every mip),
/// spread over the job pool. Pure CPU. False if `cancelled` was set.
bool compressModelImpostors(ModelData& data, const std::atomic<bool>* cancelled = nullptr);

/// What cookModel did.
struct ModelCookResult {
    bool ok = false;               ///< The cooked file was written.
    std::string error;             ///< Why not (when !ok).
    uint32_t meshes = 0;           ///< Meshes in the model.
    uint32_t generatedLevels = 0;  ///< LOD levels generated.
    uint32_t impostors = 0;        ///< Impostors baked.
    uint64_t bytes = 0;            ///< Size of the cooked file.
    double milliseconds = 0;       ///< How long cooking took.
};
/// Cooks `source` into `output`. When the settings ask for LODs / impostors
/// this needs a running Engine (its renderer and asset manager) and the main
/// thread; otherwise it is pure CPU (any thread).
ModelCookResult cookModel(const std::filesystem::path& source, const ModelSettings& settings, const std::filesystem::path& output,
                          const std::atomic<bool>* cancelled = nullptr);

} // namespace ve
