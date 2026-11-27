#include "ve/assets/ModelCook.hpp"

#include "ve/assets/AssetManager.hpp"
#include "ve/assets/Ktx2.hpp"
#include "ve/assets/TextureCook.hpp"
#include "ve/core/Engine.hpp"
#include "ve/core/FileSystem.hpp"
#include "ve/core/Jobs.hpp"
#include "ve/assets/Model.hpp"

#include <cgltf.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>

namespace ve {

namespace {

constexpr char kMagic[8] = {'V', 'E', 'M', 'O', 'D', 'E', 'L', '\0'};

uint64_t fileTime(const std::filesystem::path& file)
{
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(file, ec);
    return ec ? 0 : uint64_t(t.time_since_epoch().count());
}

ModelLodGenerator& generatorSlot()
{
    static ModelLodGenerator g;
    return g;
}

std::string lowerExtension(const std::filesystem::path& file)
{
    std::string ext = file.extension().string();
    for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

} // namespace

bool isCookableModel(const std::filesystem::path& file)
{
    const std::string ext = lowerExtension(file);
    return ext == ".gltf" || ext == ".glb";
}

std::filesystem::path cookedModelPath(const std::filesystem::path& cacheDir, const std::string& assetPath)
{
    return cacheDir / (assetPath + ".vemodel");
}

std::vector<std::filesystem::path> modelDependencies(const std::filesystem::path& source)
{
    std::vector<std::filesystem::path> deps;
    if (lowerExtension(source) != ".gltf") return deps;
    const auto bytes = vfs::read(source);
    if (!bytes) return deps;
    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse(&options, bytes->data(), bytes->size(), &data) != cgltf_result_success) return deps;
    for (size_t i = 0; i < data->buffers_count; ++i) {
        const char* uri = data->buffers[i].uri;
        if (!uri || std::strncmp(uri, "data:", 5) == 0) continue;
        std::string u = uri;
        cgltf_decode_uri(u.data());
        u.resize(std::strlen(u.c_str()));
        deps.push_back((source.parent_path() / u).lexically_normal());
    }
    cgltf_free(data);
    return deps;
}

nlohmann::json makeModelStamp(const std::filesystem::path& source, const ModelSettings& settings,
                              const std::vector<std::filesystem::path>& dependencies)
{
    nlohmann::json stamp = makeCookStamp(source, kModelCookerVersion, settings.cookHash(), settings.toJson());
    nlohmann::json deps = nlohmann::json::array();
    std::error_code ec;
    for (const std::filesystem::path& d : dependencies) {
        // Stored relative to the model's folder, so the check works wherever the project lives.
        deps.push_back({{"path", d.lexically_relative(source.parent_path()).generic_string()},
                        {"size", uint64_t(std::filesystem::file_size(d, ec))},
                        {"time", fileTime(d)},
                        {"hash", cookHex(hashFileContent(d))}});
    }
    stamp["dependencies"] = std::move(deps);
    return stamp;
}

CookedState checkCookedModel(const std::filesystem::path& cooked, const std::filesystem::path& source, const ModelSettings& settings)
{
    nlohmann::json stamp;
    if (!readCookedModel(cooked, &stamp, nullptr)) return CookedState::Unusable;
    const uint64_t hash = settings.cookHash();
    std::error_code ec;
    const bool haveSource = !source.empty() && std::filesystem::exists(source, ec);
    const CookedState state = checkCookStamp(stamp, source, kModelCookerVersion, haveSource ? &hash : nullptr);
    if (state != CookedState::Current || !haveSource) return state;
    for (const nlohmann::json& d : stamp.value("dependencies", nlohmann::json::array())) {
        const std::filesystem::path file = (source.parent_path() / d.value("path", std::string())).lexically_normal();
        if (!std::filesystem::exists(file, ec)) return CookedState::Stale;
        if (d.value("size", uint64_t(0)) != std::filesystem::file_size(file, ec)) return CookedState::Stale;
        if (d.value("time", uint64_t(0)) == fileTime(file)) continue;
        if (d.value("hash", std::string()) != cookHex(hashFileContent(file))) return CookedState::Stale;
    }
    return CookedState::Current;
}

bool writeCookedModel(const std::filesystem::path& file, const nlohmann::json& stamp, const ModelData& data, std::string* error)
{
    BinaryWriter w;
    w.putBytes(kMagic, sizeof(kMagic));
    w.put<uint32_t>(kModelCookerVersion);
    w.putString(stamp.dump());
    data.write(w);
    return writeFileReplacing(file, w.data().data(), w.data().size(), error);
}

bool readCookedModel(const std::filesystem::path& file, nlohmann::json* stamp, ModelData* data, std::string* error)
{
    // (Header only: the stamp is near the start, but the file is read whole -
    // cooked models are small next to what parsing a glTF costs.)
    const auto bytes = vfs::read(file);
    if (!bytes) {
        if (error) *error = "can't read " + file.string();
        return false;
    }
    BinaryReader r(*bytes);
    char magic[8] = {};
    for (char& c : magic) c = r.get<char>();
    if (std::memcmp(magic, kMagic, sizeof(kMagic)) != 0 || r.get<uint32_t>() != kModelCookerVersion) {
        if (error) *error = "not a current cooked model";
        return false;
    }
    const std::string stampText = r.getString();
    if (stamp) *stamp = nlohmann::json::parse(stampText, nullptr, false);
    if (data && !data->read(r)) {
        if (error) *error = "truncated cooked model";
        return false;
    }
    return r.ok();
}

void setModelLodGenerator(ModelLodGenerator generator)
{
    generatorSlot() = std::move(generator);
}

const ModelLodGenerator& modelLodGenerator()
{
    return generatorSlot();
}

bool compressModelImpostors(ModelData& data, const std::atomic<bool>* cancelled)
{
    struct Job {
        ModelImpostorData* impostor;
        bool normal;
    };
    std::vector<Job> work;
    for (ModelVariantLods& v : data.lods) {
        ModelImpostorData& i = v.impostor;
        if (!i.valid() || i.albedoFormat != VK_FORMAT_R8G8B8A8_SRGB) continue; // already compressed (or none)
        work.push_back({&i, false});
        work.push_back({&i, true});
    }
    if (work.empty()) return true;
    // Each (impostor, atlas) on its own worker; the levels of one atlas in order.
    std::vector<std::vector<uint8_t>> results(work.size());
    std::vector<std::vector<uint64_t>> offsets(work.size());
    std::atomic<bool> failed{false};
    jobs::parallelFor(work.size(), [&](size_t k) {
        const ModelImpostorData& i = *work[k].impostor;
        const std::vector<uint8_t>& src = work[k].normal ? i.normal : i.albedo;
        const VkFormat format = work[k].normal ? VK_FORMAT_BC7_UNORM_BLOCK : VK_FORMAT_BC7_SRGB_BLOCK;
        for (size_t m = 0; m < i.levelOffsets.size(); ++m) {
            const uint32_t side = std::max(1u, i.size >> m);
            offsets[k].push_back(results[k].size());
            if (!encodeRgbaLevel(src.data() + i.levelOffsets[m], side, side, format, results[k], cancelled)) {
                failed = true;
                return;
            }
        }
    });
    if (failed) return false;
    for (size_t k = 0; k < work.size(); ++k) {
        ModelImpostorData& i = *work[k].impostor;
        if (work[k].normal) {
            i.normal = std::move(results[k]);
            i.normalFormat = VK_FORMAT_BC7_UNORM_BLOCK;
        } else {
            i.albedo = std::move(results[k]);
            i.albedoFormat = VK_FORMAT_BC7_SRGB_BLOCK;
            i.levelOffsets = offsets[k]; // (same layout for both atlases)
        }
    }
    return true;
}

ModelCookResult cookModel(const std::filesystem::path& source, const ModelSettings& settings, const std::filesystem::path& output,
                          const std::atomic<bool>* cancelled)
{
    const auto t0 = std::chrono::steady_clock::now();
    ModelCookResult r;
    try {
        std::vector<std::filesystem::path> deps = modelDependencies(source);
        ModelData data = parseGltf(source);
        // Material remaps are decided now: the .model file doesn't ship.
        for (ModelMaterialData& m : data.materials)
            if (auto it = settings.materials.find(m.name); it != settings.materials.end()) m.file = it->second;

        if (settings.generatesLods()) {
            if (!Engine::exists()) {
                r.error = "LODs / impostors need the renderer (cook it from vecook or the game)";
                return r;
            }
            if (!modelLodGenerator()) {
                r.error = "no LOD generator registered (the world module registers it)";
                return r;
            }
            AssetManager& assets = Engine::get().assets();
            std::vector<std::filesystem::path> materialFiles;
            auto model = buildModel(Engine::get().renderer(), data, &assets, source, settings, &materialFiles);
            for (const std::filesystem::path& f : materialFiles) {
                try {
                    deps.push_back(assets.resolve(f));
                } catch (const std::exception&) {
                }
            }
            if (!modelLodGenerator()(data, *model, settings, r.error)) return r;
            if (!compressModelImpostors(data, cancelled)) {
                r.error = "cancelled";
                return r;
            }
        }
        const nlohmann::json stamp = makeModelStamp(source, settings, deps);
        if (!writeCookedModel(output, stamp, data, &r.error)) return r;
        r.ok = true;
        r.meshes = uint32_t(data.meshes.size());
        for (const ModelVariantLods& v : data.lods) {
            r.generatedLevels += uint32_t(v.generated.size());
            r.impostors += v.impostor.valid() ? 1u : 0u;
        }
        std::error_code ec;
        r.bytes = std::filesystem::file_size(output, ec);
    } catch (const std::exception& e) {
        r.error = e.what();
    }
    r.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

} // namespace ve
