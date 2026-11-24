#include "ve/world/Terrain.hpp"

#include "ve/assets/AssetManager.hpp"
#include "ve/assets/DataAsset.hpp"
#include "ve/core/Engine.hpp"
#include "ve/core/Log.hpp"
#include "ve/core/Profiler.hpp"
#include "ve/core/Strings.hpp"
#include "ve/physics/Physics.hpp"
#include "ve/render/Material.hpp"
#include "ve/render/Mesh.hpp"
#include "ve/render/Renderer.hpp"
#include "ve/render/Texture.hpp"
#include "ve/scene/Components.hpp"
#include "ve/scene/Scene.hpp"
#include "ve/scene/SceneSerializer.hpp"
#include "ve/world/Noise.hpp"
#include "ve/world/TerrainLod.hpp"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <chrono>
#include <cmath>

namespace ve {

namespace {
float smooth(float e0, float e1, float x) { return glm::smoothstep(e0, e1, x); }
glm::vec3 vec3From(const Json& j, glm::vec3 fallback) { return detail::vecFromJson(j, fallback); }
} // namespace

// -----------------------------------------------------------------------------
// TerrainData
// -----------------------------------------------------------------------------

namespace {

std::atomic<uint64_t> g_terrainUid{1};

constexpr int kTileSamples = TerrainData::kTileCells + 1; // per tile side (neighbours share their border)

int floorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

} // namespace

// -----------------------------------------------------------------------------
// TerrainEdits
// -----------------------------------------------------------------------------

float TerrainEdits::height(int gi, int gj) const
{
    if (tiles.empty()) return 0.0f;
    const int tx = floorDiv(gi, kTile), tz = floorDiv(gj, kTile);
    const auto it = tiles.find(key(tx, tz));
    if (it == tiles.end()) return 0.0f;
    return it->second->height[size_t(gj - tz * kTile) * kSide + size_t(gi - tx * kTile)];
}

void TerrainEdits::paint(int gi, int gj, int& biome, float& strength) const
{
    biome = -1;
    strength = 0.0f;
    if (tiles.empty()) return;
    const int tx = floorDiv(gi, kTile), tz = floorDiv(gj, kTile);
    const auto it = tiles.find(key(tx, tz));
    if (it == tiles.end()) return;
    const size_t k = size_t(gj - tz * kTile) * kSide + size_t(gi - tx * kTile);
    if (it->second->biome[k] == 255 || it->second->paint[k] == 0) return;
    biome = it->second->biome[k];
    strength = float(it->second->paint[k]) / 255.0f;
}

TerrainData::TerrainData(const TerrainShape& shape, glm::vec3 centre, std::vector<TerrainBiome> biomes,
                         std::vector<std::pair<glm::vec3, TerrainStampShape>> stamps, size_t cacheBytes, TerrainEdits edits)
    : m_shape(shape), m_centre(centre), m_biomes(std::move(biomes)), m_edits(std::move(edits)), m_uid(g_terrainUid++)
{
    cellSize = std::max(shape.cellSize, 0.1f);
    res = std::max(2, int(std::round(shape.size / cellSize)) + 1);
    m_half = (res - 1) / 2;
    origin = glm::vec2(centre.x, centre.z) - glm::vec2(float(res - 1) * cellSize * 0.5f);
    for (const auto& b : m_biomes) biomeNames.push_back(b.name);
    if (biomeNames.empty()) biomeNames.push_back("Default");
    for (auto& [pos, st] : stamps) {
        const int biome = st.biome.empty() ? -1 : biomeIndex(st.biome);
        m_stamps.push_back({pos, std::move(st), biome});
    }
    const size_t tileBytes = size_t(kTileSamples) * kTileSamples * sizeof(float) * (1 + biomeNames.size());
    m_maxTiles = std::max<size_t>(16, cacheBytes / tileBytes);
}

float TerrainData::generateHeight(glm::vec2 p) const
{
    return baseHeight(p) + editHeight(p);
}

float TerrainData::editHeight(glm::vec2 p) const
{
    if (m_edits.empty()) return 0.0f;
    const glm::vec2 u = (p - origin) / cellSize;
    const int i = int(std::floor(u.x)), j = int(std::floor(u.y));
    const float fx = u.x - float(i), fz = u.y - float(j);
    const int gi = i - m_half, gj = j - m_half;
    const float h00 = m_edits.height(gi, gj), h10 = m_edits.height(gi + 1, gj);
    const float h01 = m_edits.height(gi, gj + 1), h11 = m_edits.height(gi + 1, gj + 1);
    return glm::mix(glm::mix(h00, h10, fx), glm::mix(h01, h11, fx), fz);
}

void TerrainData::adoptTiles(const TerrainData& old, glm::vec4 changed)
{
    if (old.res != res || old.cellSize != cellSize || old.origin != origin || old.biomeNames != biomeNames) return;
    // Biome weights depend on the slope, which reaches one sample further.
    const float margin = 2.0f * cellSize, tileSize = float(kTileCells) * cellSize;
    std::scoped_lock lock(m_mutex, old.m_mutex);
    for (const auto& [key, cached] : old.m_tiles) {
        const glm::ivec2 t = terrainTileCoords(key);
        const glm::vec2 lo = origin + glm::vec2(t) * tileSize;
        const bool touched = lo.x - margin <= changed.z && lo.x + tileSize + margin >= changed.x &&
                             lo.y - margin <= changed.w && lo.y + tileSize + margin >= changed.y;
        if (!touched) m_tiles.emplace(key, Cached{cached.tile, cached.lastUse});
    }
    m_clock = old.m_clock;
}

float TerrainData::baseHeight(glm::vec2 p) const
{
    const TerrainShape& t = m_shape;
    const auto seedU = static_cast<uint32_t>(t.seed);
    float h = t.baseHeight + (noise::fbm(p * t.noiseFrequency, t.noiseOctaves, seedU) - 0.5f) * t.noiseAmplitude +
              std::sin(p.x * t.waves.y) * std::cos(p.y * t.waves.z) * t.waves.x;
    for (const Stamp& st : m_stamps) {
        const TerrainStampShape& s = st.s;
        const float dist = glm::length(p - glm::vec2(st.pos.x, st.pos.z));
        const float w = 1.0f - smooth(s.radius, s.radius + std::max(s.falloff, 0.01f), dist);
        if (w <= 0.0f) continue;
        if (s.mode == TerrainStampShape::Mode::Flatten) {
            float target = st.pos.y;
            if (s.noiseAmplitude != 0.0f)
                target += noise::fbm(p * s.noiseFrequency, 3, static_cast<uint32_t>(s.noiseSeed)) * s.noiseAmplitude;
            h = glm::mix(h, target, w);
        } else {
            float add = std::pow(w, std::max(s.exponent, 0.01f)) * s.height;
            if (s.ridgeAmount != 0.0f) {
                const float ridges = 1.0f - std::abs(noise::fbm(p * s.ridgeFrequency, 3, static_cast<uint32_t>(s.ridgeSeed)) * 2.0f - 1.0f);
                add += w * ridges * s.ridgeAmount;
            }
            h += add;
        }
    }
    const float edgeDist = std::max(std::abs(p.x - m_centre.x), std::abs(p.y - m_centre.z));
    const float edge = smooth(t.borderStart, t.borderEnd, edgeDist);
    if (edge > 0.0f) h += edge * edge * (t.borderHeight + noise::fbm(p * t.borderNoiseFrequency, 3, seedU + 22u) * t.borderNoise);
    return h + m_centre.y;
}

void TerrainData::generateBiomes(glm::vec2 p, float h, float slope, float* w) const
{
    const size_t nb = biomeNames.size();
    if (m_biomes.size() <= 1) {
        w[0] = 1.0f;
        return;
    }
    // Biome rules: height range, slope, noise coverage, then stamps paint over them.
    for (size_t b = 1; b < m_biomes.size(); ++b) {
        const TerrainBiome& bio = m_biomes[b];
        float r = 0.0f;
        if (bio.coverage > 0.0f) {
            r = smooth(bio.minHeight - 0.5f, bio.minHeight + 0.5f, h) * (1.0f - smooth(bio.maxHeight - 0.5f, bio.maxHeight + 0.5f, h)) *
                (1.0f - smooth(bio.maxSlope - 0.05f, bio.maxSlope + 0.05f, slope));
            if (bio.coverageFrequency > 0.0f && bio.coverage < 1.0f) {
                const float nz = noise::fbm(p * bio.coverageFrequency, 3, static_cast<uint32_t>(bio.coverageSeed));
                r *= smooth(1.0f - bio.coverage - 0.05f, 1.0f - bio.coverage + 0.05f, nz);
            }
        }
        w[b] = r;
    }
    for (const Stamp& st : m_stamps) {
        if (st.biome <= 0) continue;
        const float dist = glm::length(p - glm::vec2(st.pos.x, st.pos.z));
        const float k = st.s.biomeStrength * (1.0f - smooth(st.s.biomeRadius, st.s.biomeRadius + std::max(st.s.biomeFalloff, 0.01f), dist));
        w[size_t(st.biome)] = std::max(w[size_t(st.biome)], k);
    }
    // Later biomes paint over earlier ones; biome 0 gets what's left.
    float remaining = 1.0f;
    for (size_t b = nb - 1; b >= 1; --b) {
        const float v = std::clamp(w[b], 0.0f, 1.0f) * remaining;
        w[b] = v;
        remaining -= v;
    }
    w[0] = remaining;

    // Painted biomes (editor brush) blend over the rules.
    if (m_edits.empty()) return;
    const glm::vec2 u = (p - origin) / cellSize;
    const int i = int(std::floor(u.x)), j = int(std::floor(u.y));
    const float fx = u.x - float(i), fz = u.y - float(j);
    thread_local std::vector<float> painted;
    painted.assign(nb, 0.0f);
    float total = 0.0f;
    const float cw[4] = {(1 - fx) * (1 - fz), fx * (1 - fz), (1 - fx) * fz, fx * fz};
    for (int c = 0; c < 4; ++c) {
        int biome;
        float strength;
        m_edits.paint(i + (c & 1) - m_half, j + (c >> 1) - m_half, biome, strength);
        if (biome < 0 || size_t(biome) >= nb || cw[c] <= 0.0f) continue;
        painted[size_t(biome)] += strength * cw[c];
        total += strength * cw[c];
    }
    if (total <= 0.0f) return;
    const float keep = 1.0f - std::min(total, 1.0f);
    for (size_t b = 0; b < nb; ++b) w[b] = w[b] * keep + painted[b];
}

glm::vec3 TerrainData::groundColor(const TerrainLook& look, glm::vec2 p, float h, glm::vec3 n, const float* weights,
                                   float* rockOut, float* mudOut) const
{
    glm::vec3 c(0.0f);
    for (size_t b = 0; b < m_biomes.size(); ++b) {
        const float w = weights[b];
        if (w <= 0.0f) continue;
        const TerrainBiome& bio = m_biomes[b];
        glm::vec3 bc = bio.color;
        if (bio.color2 != bio.color && bio.colorNoiseFrequency > 0.0f)
            bc = glm::mix(bio.color, bio.color2, smooth(0.45f, 0.7f, noise::fbm(p * bio.colorNoiseFrequency, 3, static_cast<uint32_t>(bio.colorNoiseSeed))));
        if (bio.patchFrequency > 0.0f) {
            const float n2 = noise::fbm(p * bio.patchFrequency + 3.0f, 3, static_cast<uint32_t>(bio.patchSeed));
            bc = glm::mix(bc, bio.patchColor, smooth(1.0f - bio.patchCoverage - 0.05f, 1.0f - bio.patchCoverage + 0.05f, n2) * bio.patchStrength);
        }
        c += bc * w;
    }
    if (m_biomes.empty()) c = glm::vec3(0.5f);
    c *= 1.0f - look.brightnessVariation * 0.5f + look.brightnessVariation * noise::fbm(p * 0.15f, 2, 51);
    const float mud = smooth(look.waterLevel + 1.0f, look.waterLevel - 0.2f, h);
    c = glm::mix(c, look.mudColor, mud);
    const float steep = smooth(look.steepStart, look.steepEnd, 1.0f - n.y);
    const float edge = smooth(look.borderRockStart, look.borderRockEnd, std::max(std::abs(p.x - m_centre.x), std::abs(p.y - m_centre.z)));
    const float rock = std::max(steep, edge * look.borderRock);
    if (rockOut) *rockOut = rock;
    if (mudOut) *mudOut = mud;
    return glm::mix(c, look.rockColor, rock);
}

std::shared_ptr<const TerrainData::Tile> TerrainData::generateTile(int tx, int tz) const
{
    // Heights with a one-sample apron (for the slopes at the tile's border).
    constexpr int N = kTileSamples, A = N + 2;
    const int i0 = tx * kTileCells, j0 = tz * kTileCells;
    auto clampIdx = [&](int v) { return std::clamp(v, 0, res - 1); };
    std::vector<float> apron(size_t(A) * A);
    for (int jj = 0; jj < A; ++jj)
        for (int ii = 0; ii < A; ++ii)
            apron[size_t(jj) * A + ii] = generateHeight(samplePosition(clampIdx(i0 + ii - 1), clampIdx(j0 + jj - 1)));
    auto ah = [&](int ii, int jj) { return apron[size_t(jj + 1) * A + size_t(ii + 1)]; }; // ii, jj in -1..N

    auto tile = std::make_shared<Tile>();
    tile->heights.resize(size_t(N) * N);
    const size_t nb = biomeNames.size();
    tile->biomes.assign(nb, std::vector<float>(size_t(N) * N, 0.0f));
    std::vector<float> w(nb);
    const float e = cellSize;
    for (int jj = 0; jj < N; ++jj) {
        for (int ii = 0; ii < N; ++ii) {
            const size_t idx = size_t(jj) * N + ii;
            const float h = ah(ii, jj);
            tile->heights[idx] = h;
            if (m_biomes.size() <= 1) {
                tile->biomes[0][idx] = 1.0f;
                continue;
            }
            const glm::vec2 p = samplePosition(clampIdx(i0 + ii), clampIdx(j0 + jj));
            const glm::vec3 n = glm::normalize(glm::vec3(ah(ii - 1, jj) - ah(ii + 1, jj), 2.0f * e, ah(ii, jj - 1) - ah(ii, jj + 1)));
            generateBiomes(p, h, 1.0f - n.y, w.data());
            for (size_t b = 0; b < nb; ++b) tile->biomes[b][idx] = w[b];
        }
    }
    return tile;
}

std::shared_ptr<const TerrainData::Tile> TerrainData::tile(int tx, int tz) const
{
    // Per-thread memo of the last few tiles: most queries hit the same one, without the lock.
    struct Memo {
        uint64_t uid = 0;
        int64_t key = 0;
        std::shared_ptr<const Tile> tile;
    };
    thread_local Memo memo[4];
    thread_local unsigned next = 0;
    const int64_t key = terrainTileKey(tx, tz);
    for (const Memo& m : memo)
        if (m.uid == m_uid && m.key == key) return m.tile;

    std::shared_ptr<const Tile> t;
    {
        std::lock_guard lock(m_mutex);
        if (auto it = m_tiles.find(key); it != m_tiles.end()) {
            it->second.lastUse = ++m_clock;
            t = it->second.tile;
        }
    }
    if (!t) {
        t = generateTile(tx, tz); // outside the lock: other threads keep querying
        std::lock_guard lock(m_mutex);
        auto [it, fresh] = m_tiles.try_emplace(key, Cached{t, ++m_clock});
        if (!fresh) t = it->second.tile; // another thread was faster
        if (m_tiles.size() > m_maxTiles) {
            // Evict the least recently used tile (holders keep theirs alive).
            auto oldest = m_tiles.begin();
            for (auto i = m_tiles.begin(); i != m_tiles.end(); ++i)
                if (i->second.lastUse < oldest->second.lastUse) oldest = i;
            if (oldest->first != key) m_tiles.erase(oldest);
        }
    }
    memo[next] = {m_uid, key, t};
    next = (next + 1) % 4;
    return t;
}

std::shared_ptr<const TerrainData::Tile> TerrainData::tileFor(int& i, int& j) const
{
    // The tile whose 65 x 65 samples contain (i, j) and (i + 1, j + 1) for i < res - 1.
    i = std::clamp(i, 0, res - 1);
    j = std::clamp(j, 0, res - 1);
    const int tx = std::min(i, res - 2) / kTileCells, tz = std::min(j, res - 2) / kTileCells;
    i -= tx * kTileCells;
    j -= tz * kTileCells;
    return tile(tx, tz);
}

size_t TerrainData::cachedTiles() const
{
    std::lock_guard lock(m_mutex);
    return m_tiles.size();
}

float TerrainData::sampleHeight(int i, int j) const
{
    const auto t = tileFor(i, j);
    return t->heights[size_t(j) * kTileSamples + size_t(i)];
}

float TerrainData::sampleBiome(int biome, int i, int j) const
{
    if (biome < 0 || biome >= int(biomeNames.size())) return 0.0f;
    const auto t = tileFor(i, j);
    return t->biomes[size_t(biome)][size_t(j) * kTileSamples + size_t(i)];
}

float TerrainData::height(float x, float z) const
{
    if (res < 2) return 0.0f;
    const float fx = std::clamp((x - origin.x) / cellSize, 0.0f, float(res - 1) - 0.001f);
    const float fz = std::clamp((z - origin.y) / cellSize, 0.0f, float(res - 1) - 0.001f);
    int ix = static_cast<int>(fx), iz = static_cast<int>(fz);
    const float tx = fx - float(ix), tz = fz - float(iz);
    const auto t = tileFor(ix, iz); // the cell's four samples are all in this tile
    auto at = [&](int i, int j) { return t->heights[size_t(j) * kTileSamples + size_t(i)]; };
    // Same triangles as the render mesh and Jolt's heightfield: each cell is
    // split along its (i, j) -> (i + 1, j + 1) diagonal.
    const float h00 = at(ix, iz), h11 = at(ix + 1, iz + 1);
    if (tx >= tz) return h00 + (at(ix + 1, iz) - h00) * tx + (h11 - at(ix + 1, iz)) * tz;
    return h00 + (at(ix, iz + 1) - h00) * tz + (h11 - at(ix, iz + 1)) * tx;
}

glm::vec3 TerrainData::normal(float x, float z) const
{
    const float e = cellSize;
    const float hl = height(x - e, z), hr = height(x + e, z);
    const float hd = height(x, z - e), hu = height(x, z + e);
    return glm::normalize(glm::vec3(hl - hr, 2.0f * e, hd - hu));
}

float TerrainData::biomeWeight(int biome, float x, float z) const
{
    if (biome < 0 || biome >= int(biomeNames.size()) || res < 2) return 0.0f;
    const float fx = std::clamp((x - origin.x) / cellSize, 0.0f, float(res - 1) - 0.001f);
    const float fz = std::clamp((z - origin.y) / cellSize, 0.0f, float(res - 1) - 0.001f);
    int ix = static_cast<int>(fx), iz = static_cast<int>(fz);
    const float tx = fx - float(ix), tz = fz - float(iz);
    const auto t = tileFor(ix, iz);
    const auto& w = t->biomes[size_t(biome)];
    auto at = [&](int i, int j) { return w[size_t(j) * kTileSamples + size_t(i)]; };
    return glm::mix(glm::mix(at(ix, iz), at(ix + 1, iz), tx), glm::mix(at(ix, iz + 1), at(ix + 1, iz + 1), tx), tz);
}

int TerrainData::biomeIndex(const std::string& name) const
{
    for (size_t i = 0; i < biomeNames.size(); ++i) {
        if (biomeNames[i] == name) return int(i);
    }
    return -1;
}

bool TerrainData::inBounds(float x, float z, float margin) const
{
    const float extent = float(res - 1) * cellSize;
    return x >= origin.x + margin && z >= origin.y + margin && x <= origin.x + extent - margin && z <= origin.y + extent - margin;
}

// -----------------------------------------------------------------------------
// Terrain
// -----------------------------------------------------------------------------

namespace {
// Live terrains, so find() (called every frame by ambience, foliage, the
// player...) doesn't scan every scene object with dynamic_cast. Main thread only.
std::vector<Terrain*>& liveTerrains()
{
    static std::vector<Terrain*> list;
    return list;
}
} // namespace

Terrain::Terrain() : GameObject("Terrain")
{
    runInEditor = true; // rebuilds while you edit stamps / fields
    liveTerrains().push_back(this);
}

Terrain::~Terrain()
{
    std::erase(liveTerrains(), this);
}

Terrain* Terrain::find(const Scene& scene)
{
    // The first one spawned wins, as when scanning the scene's objects.
    for (Terrain* t : liveTerrains()) {
        if (!t->isDestroyed() && t->inScene(scene)) return t;
    }
    return nullptr;
}

std::shared_ptr<const TerrainData> Terrain::data() const
{
    std::lock_guard lock(m_mutex);
    if (!m_data) {
        m_data = const_cast<Terrain*>(this)->generate(); // first use (main thread)
        const_cast<Terrain*>(this)->m_editState = editState().dump();
    }
    return m_data;
}

float Terrain::biomeWeight(const std::string& biome, float x, float z) const
{
    auto d = data();
    return d->biomeWeight(d->biomeIndex(biome), x, z);
}

glm::vec2 Terrain::boundsMin() const
{
    auto d = data();
    return d->origin;
}

glm::vec2 Terrain::boundsMax() const
{
    auto d = data();
    return d->origin + glm::vec2(float(d->res - 1) * d->cellSize);
}

Json Terrain::editState() const
{
    // Everything that affects the terrain (to notice edits): its own fields and the stamps.
    Json j = Json::array();
    if (const TypeInfo* self = TypeRegistry::get().find("Terrain")) {
        Json fields = SceneSerializer::serializeFields(*self, this);
        if (fields.is_object()) fields.erase("edits"); // brush edits are tracked separately (m_editsVersion)
        j.push_back(std::move(fields));
    }
    const glm::vec3 c = transform().position();
    j.push_back({c.x, c.y, c.z});
    const TypeInfo* t = TypeRegistry::get().find("TerrainStamp");
    scene().forEach<TerrainStamp>([&](TerrainStamp& s) {
        if (!s.isActive()) return;
        const glm::vec3 p = s.transform().position();
        j.push_back({p.x, p.y, p.z, t ? SceneSerializer::serializeFields(*t, &s) : Json()});
    });
    return j;
}

std::shared_ptr<TerrainData> Terrain::generate(bool quiet) const
{
    // A snapshot of the inputs: samples are generated lazily, tile by tile.
    std::vector<std::pair<glm::vec3, TerrainStampShape>> stamps;
    scene().forEach<TerrainStamp>([&](TerrainStamp& s) {
        if (s.isActive()) stamps.push_back({s.transform().position(), s});
    });
    const size_t stampCount = stamps.size();
    auto d = std::make_shared<TerrainData>(*this, transform().position(), biomes, std::move(stamps),
                                           size_t(std::max(tileCacheMB, 1.0f) * 1024.0f * 1024.0f), m_edits);
    if (!quiet)
        log::info("Terrain: {:.0f} m, {}x{} samples in tiles of {} cells (generated on demand), {} biomes, {} stamps",
              float(d->res - 1) * d->cellSize, d->res, d->res, TerrainData::kTileCells, d->biomeNames.size(), stampCount);
    return d;
}

void Terrain::start()
{
    m_started = true;
    data();
    m_lod = std::make_unique<TerrainLod>(renderer());
    rebuild(); // materials, LOD data, colliders; preloads the view
}

void Terrain::update(float dt)
{
    VE_PROFILE("Terrain");
    m_sinceRebuild += dt;
    m_sinceCheck += dt;
    m_sinceColliders += dt;
    // Serializing every field (stamps, biomes) is too costly for every frame;
    // edits are picked up within a quarter second (code can call markDirty()).
    if (m_sinceCheck >= 0.25f) {
        m_sinceCheck = 0.0f;
        refreshBiomeAssets(); // a .biome file was edited (editor or on disk)
        if (editState().dump() != m_editState) m_dirty = true;
    }
    if (m_dirty && m_sinceRebuild > 0.25f) rebuild();
    updateNodes(false);
    if (m_sinceColliders >= 0.1f) updateColliders(false);
}

void Terrain::rebuild()
{
    m_dirty = false;
    m_sinceRebuild = 0.0f;
    // Only brush edits since the last rebuild: everything outside the brushed
    // area (sample tiles, LOD meshes, colliders, foliage, grass) stays.
    std::string state = editState().dump();
    const glm::vec4 area = m_editArea;
    const bool editsOnly = m_data && state == m_editState && !m_editsReplaced && area.x <= area.z;
    std::shared_ptr<const TerrainData> old = m_data;
    auto fresh = generate(editsOnly);
    if (editsOnly) fresh->adoptTiles(*old, area);
    {
        std::lock_guard lock(m_mutex);
        m_data = fresh;
    }
    m_editState = std::move(state);
    ++m_version;
    m_changes.push_back({m_version, editsOnly, area});
    if (m_changes.size() > 64) m_changes.erase(m_changes.begin());
    m_editArea = glm::vec4(1e30f, 1e30f, -1e30f, -1e30f);
    m_editsReplaced = false;
    if (!m_started) return;
    TerrainLod::Settings ls;
    ls.nodeCells = chunkCells;
    ls.lodFactor = std::max(lodFactor, 1.1f);
    m_lod->setData(fresh, *this, ls, editsOnly ? &area : nullptr);
    updateMaterials();
    if (engine().loading() && !m_loadTask) {
        // Behind a loading screen: meshes come from the worker over the next
        // frames (colliders follow in update()) instead of blocking here.
        m_loadTask = true;
        updateNodes(false);
        const uint64_t id = this->id();
        Scene* s = &scene();
        engine().addLoadTask("Shaping the land", [s, id] {
            auto* t = dynamic_cast<Terrain*>(s->findById(id));
            return t ? t->loadProgress() : 1.0f;
        });
        return;
    }
    updateColliders(!editsOnly, editsOnly ? &area : nullptr);
    updateNodes(!editsOnly);
}

float Terrain::loadProgress() const
{
    if (!m_lod) return 0.0f;
    const TerrainLod::Stats& s = m_lod->stats();
    if (s.missing == 0 && s.pending == 0) return 1.0f;
    return std::min(0.99f, float(s.drawn) / float(s.drawn + s.missing + s.pending));
}

bool Terrain::changedOnlyIn(uint64_t version, glm::vec4& area) const
{
    area = glm::vec4(1e30f, 1e30f, -1e30f, -1e30f);
    if (m_changes.empty() || m_changes.front().version > version + 1) return false; // history doesn't reach back
    for (const Change& c : m_changes) {
        if (c.version <= version) continue;
        if (!c.editsOnly) return false;
        area = glm::vec4(glm::min(glm::vec2(area), glm::vec2(c.area)), glm::max(glm::vec2(area.z, area.w), glm::vec2(c.area.z, c.area.w)));
    }
    return area.x <= area.z;
}

// -----------------------------------------------------------------------------
// Sculpting & painting
// -----------------------------------------------------------------------------

void Terrain::applyBrush(const Brush& brush, glm::vec2 centre, float dt)
{
    const auto d = data();
    const float cs = d->cellSize, r = std::max(brush.radius, cs);
    const glm::vec2 u = (centre - d->origin) / cs;
    const int i0 = std::max(0, int(std::floor(u.x - r / cs)) - 1), i1 = std::min(d->res - 1, int(std::ceil(u.x + r / cs)) + 1);
    const int j0 = std::max(0, int(std::floor(u.y - r / cs)) - 1), j1 = std::min(d->res - 1, int(std::ceil(u.y + r / cs)) + 1);
    if (i0 > i1 || j0 > j1 || dt <= 0.0f) return;
    const int w = i1 - i0 + 1, h = j1 - j0 + 1;
    constexpr int T = TerrainEdits::kTile, S = TerrainEdits::kSide;

    // Current heights (generator + edits so far): smoothing and flattening work on them.
    std::vector<float> cur(size_t(w) * h);
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i) {
            const glm::ivec2 g = d->editCoord(i, j);
            cur[size_t(j - j0) * w + size_t(i - i0)] = d->baseHeight(d->samplePosition(i, j)) + m_edits.height(g.x, g.y);
        }
    auto at = [&](int i, int j) { return cur[size_t(std::clamp(j, j0, j1) - j0) * w + size_t(std::clamp(i, i0, i1) - i0)]; };

    // Tiles touched by this step are copied once (snapshots may share the old ones).
    std::unordered_map<int64_t, std::shared_ptr<TerrainEdits::Tile>> work;
    auto writable = [&](int tx, int tz) -> TerrainEdits::Tile& {
        const int64_t k = TerrainEdits::key(tx, tz);
        if (auto it = work.find(k); it != work.end()) return *it->second;
        const auto old = m_edits.tiles.find(k);
        auto t = old != m_edits.tiles.end() ? std::make_shared<TerrainEdits::Tile>(*old->second) : std::make_shared<TerrainEdits::Tile>();
        return *work.emplace(k, std::move(t)).first->second;
    };
    // A sample on a tile border belongs to both tiles (and a corner to four).
    auto forTiles = [&](glm::ivec2 g, auto&& fn) {
        const int tx = floorDiv(g.x, T), tz = floorDiv(g.y, T);
        const int lx = g.x - tx * T, lz = g.y - tz * T;
        for (int dz = 0; dz <= (lz == 0 ? 1 : 0); ++dz)
            for (int dx = 0; dx <= (lx == 0 ? 1 : 0); ++dx) fn(writable(tx - dx, tz - dz), size_t(lz + dz * T) * S + size_t(lx + dx * T));
    };

    const float rate = std::clamp(brush.strength, 0.0f, 1.0f);
    const float approach = std::min(1.0f, rate * 12.0f * dt); // smooth / flatten: fraction of the way per step
    for (int j = j0; j <= j1; ++j) {
        for (int i = i0; i <= i1; ++i) {
            const float dist = glm::length(d->samplePosition(i, j) - centre);
            if (dist >= r) continue;
            const float f = 1.0f - glm::smoothstep(r * 0.5f, r, dist);
            const glm::ivec2 g = d->editCoord(i, j);
            const float hNow = at(i, j);
            float delta = 0.0f;
            switch (brush.mode) {
            case Brush::Mode::Raise: delta = rate * 8.0f * dt * f; break;
            case Brush::Mode::Lower: delta = -rate * 8.0f * dt * f; break;
            case Brush::Mode::Smooth: {
                float sum = 0.0f;
                for (int b = -1; b <= 1; ++b)
                    for (int a = -1; a <= 1; ++a) sum += at(i + a, j + b);
                delta = (sum / 9.0f - hNow) * approach * f;
                break;
            }
            case Brush::Mode::Flatten: delta = (brush.height - hNow) * approach * f; break;
            case Brush::Mode::Paint: {
                const float amount = rate * 4.0f * dt * f * 255.0f;
                const auto biome = uint8_t(std::clamp(brush.biome, 0, 254));
                forTiles(g, [&](TerrainEdits::Tile& t, size_t k) {
                    // Painting over another biome first fades it out.
                    float p = t.paint[k];
                    if (t.biome[k] == biome || p <= 0.0f) {
                        t.biome[k] = biome;
                        p = std::min(255.0f, p + amount);
                    } else if (p <= amount) {
                        t.biome[k] = biome;
                        p = amount - p;
                    } else {
                        p -= amount;
                    }
                    t.paint[k] = uint8_t(std::lround(p));
                });
                break;
            }
            }
            if (delta != 0.0f)
                forTiles(g, [&](TerrainEdits::Tile& t, size_t k) { t.height[k] = std::clamp(t.height[k] + delta, -300.0f, 300.0f); });
        }
    }
    for (auto& [k, t] : work) m_edits.tiles[k] = std::move(t);
    const glm::vec2 lo = d->samplePosition(i0, j0), hi = d->samplePosition(i1, j1);
    m_editArea = glm::vec4(glm::min(glm::vec2(m_editArea), lo), glm::max(glm::vec2(m_editArea.z, m_editArea.w), hi));
    ++m_editsVersion;
    m_dirty = true;
}

void Terrain::clearEdits()
{
    if (m_edits.empty()) return;
    m_edits.tiles.clear();
    m_editsReplaced = true;
    ++m_editsVersion;
    m_dirty = true;
}

Json Terrain::editsJson() const
{
    // Per edited tile: heights in cm (int16), painted biome and strength (bytes), base64.
    Json tiles = Json::array();
    std::vector<int64_t> keys;
    for (const auto& [k, t] : m_edits.tiles) keys.push_back(k);
    std::sort(keys.begin(), keys.end()); // stable files
    for (int64_t k : keys) {
        const TerrainEdits::Tile& t = *m_edits.tiles.at(k);
        std::vector<int16_t> cm(t.height.size());
        for (size_t i = 0; i < cm.size(); ++i) cm[i] = int16_t(std::clamp(std::lround(t.height[i] * 100.0f), -32767l, 32767l));
        const glm::ivec2 c = terrainTileCoords(k);
        tiles.push_back({{"x", c.x}, {"z", c.y},
                         {"height", str::base64Encode(cm.data(), cm.size() * sizeof(int16_t))},
                         {"biome", str::base64Encode(t.biome.data(), t.biome.size())},
                         {"paint", str::base64Encode(t.paint.data(), t.paint.size())}});
    }
    return Json{{"tileCells", TerrainEdits::kTile}, {"tiles", tiles}};
}

void Terrain::setEditsJson(const Json& j)
{
    TerrainEdits edits;
    if (j.is_object() && j.value("tileCells", 0) == TerrainEdits::kTile && j.contains("tiles")) {
        constexpr size_t n = size_t(TerrainEdits::kSide) * TerrainEdits::kSide;
        for (const Json& e : j["tiles"]) {
            auto t = std::make_shared<TerrainEdits::Tile>();
            const auto cm = str::base64Decode(e.value("height", std::string()));
            const auto biome = str::base64Decode(e.value("biome", std::string()));
            const auto paint = str::base64Decode(e.value("paint", std::string()));
            if (cm.size() != n * 2 || biome.size() != n || paint.size() != n) {
                log::warn("Terrain: ignoring a malformed edit tile");
                continue;
            }
            for (size_t i = 0; i < n; ++i) t->height[i] = float(int16_t(uint16_t(cm[2 * i] | cm[2 * i + 1] << 8))) / 100.0f;
            t->biome = biome;
            t->paint = paint;
            edits.tiles[TerrainEdits::key(e.value("x", 0), e.value("z", 0))] = std::move(t);
        }
    }
    if (edits.empty() && m_edits.empty()) return;
    m_edits = std::move(edits);
    m_editsReplaced = true;
    ++m_editsVersion;
    m_dirty = true;
}

void Terrain::onDestroy()
{
    if (GameObject* nodes = m_nodeObject.get(scene())) nodes->destroy();
    m_nodeObject.reset();
    m_nodeRenderers.clear();
    m_colliderTiles.clear(); // components die with this object
    m_lod.reset();
}

uint32_t Terrain::drawnNodes() const
{
    return m_lod ? m_lod->stats().drawn : 0;
}

namespace {

// Normal x/y, roughness and AO in one linear RGBA texture (two fetches per
// layer instead of four). Maps of different sizes are resampled to the largest.
std::shared_ptr<Texture> packSurfaceMap(Renderer& r, const AssetManager& assets, const TerrainMaterial& m)
{
    ImagePixels maps[3];
    const std::string* paths[3] = {&m.normal, &m.roughness, &m.ao};
    int w = 1, h = 1;
    for (int k = 0; k < 3; ++k) {
        if (paths[k]->empty()) continue;
        maps[k] = assets.loadPixels(*paths[k]); // (also from a packaged game's cooked copy)
        w = std::max(w, maps[k].width);
        h = std::max(h, maps[k].height);
    }
    std::vector<uint8_t> px(size_t(w) * h * 4);
    const uint8_t defaults[4] = {128, 128, uint8_t(std::clamp(m.roughnessValue, 0.0f, 1.0f) * 255.0f + 0.5f), 255};
    auto at = [&](const ImagePixels& img, int x, int y, int c) {
        return img.rgba[(size_t(y * img.height / h) * img.width + size_t(x * img.width / w)) * 4 + c];
    };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t* o = &px[(size_t(y) * w + x) * 4];
            o[0] = maps[0].width ? at(maps[0], x, y, 0) : defaults[0];
            o[1] = maps[0].width ? at(maps[0], x, y, 1) : defaults[1];
            o[2] = maps[1].width ? at(maps[1], x, y, 0) : defaults[2];
            o[3] = maps[2].width ? at(maps[2], x, y, 0) : defaults[3];
        }
    }
    return Texture::create2D(r, uint32_t(w), uint32_t(h), VK_FORMAT_R8G8B8A8_UNORM, px.data(), px.size(), true,
                             SamplerType::LinearRepeat, "TerrainSurface:" + m.normal);
}

} // namespace

void Terrain::updateMaterials()
{
    Renderer& r = renderer();
    if (!m_shader) {
        ShaderDesc desc;
        desc.name = "Terrain";
        desc.vertex = "terrain.vert";
        desc.fragment = "terrain.frag";
        desc.shadowVertex = "terrain_shadow.vert"; // same geomorph as the colour pass
        m_shader = r.createShader(desc);
    }
    std::shared_ptr<Texture> detail;
    if (!detailTexture.empty()) {
        try {
            detail = assets().loadTexture(detailTexture); // (data: its TextureSettings, e.g. *_detail)
        } catch (const std::exception& e) {
            log::warn("Terrain: {}", e.what());
        }
    }
    if (!detail) {
        // The shader always samples slot 0: mid-grey (0.5) is its neutral value.
        if (!m_flatDetail) {
            const uint8_t grey[4] = {128, 128, 128, 255};
            m_flatDetail = Texture::create2D(r, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, grey, sizeof(grey), false,
                                             SamplerType::LinearRepeat, "terrain flat detail");
        }
        detail = m_flatDetail;
    }
    // Splat layers (biomes, then rock): a row of parameters each in a small
    // float texture. Textures are loaded once and kept while in use.
    std::vector<const TerrainMaterial*> layers;
    for (const auto& b : biomes) layers.push_back(&b.material);
    layers.push_back(&rockMaterial);
    std::unordered_map<std::string, std::shared_ptr<Texture>> keep;
    auto layerTexture = [&](const std::string& key, auto make) -> float {
        auto it = keep.find(key);
        if (it == keep.end()) {
            auto old = m_layerTextures.find(key);
            std::shared_ptr<Texture> tex;
            try {
                tex = old != m_layerTextures.end() ? old->second : make();
            } catch (const std::exception& e) {
                log::warn("Terrain: {}", e.what());
            }
            it = keep.emplace(key, tex).first;
        }
        return it->second ? float(it->second->index()) : -1.0f;
    };
    std::vector<float> table;
    bool anyTextured = false;
    for (const TerrainMaterial* lm : layers) {
        float albedo = -1.0f, surface = -1.0f;
        if (lm->textured()) {
            anyTextured = true;
            if (!lm->albedo.empty())
                albedo = layerTexture("albedo:" + lm->albedo, [&] { return assets().loadTexture(lm->albedo); });
            if (!lm->normal.empty() || !lm->roughness.empty() || !lm->ao.empty())
                surface = layerTexture("surface:" + lm->normal + "|" + lm->roughness + "|" + lm->ao + "|" + std::to_string(lm->roughnessValue),
                                       [&] { return packSurfaceMap(r, assets(), *lm); });
        }
        table.insert(table.end(), {albedo, surface, 1.0f / std::max(lm->size, 0.01f), lm->normalStrength,
                                   lm->tint.x, lm->tint.y, lm->tint.z, lm->roughnessValue,
                                   std::clamp(lm->biomeColor, 0.0f, 1.0f), 0.0f, 0.0f, 0.0f});
    }
    m_layerTextures = std::move(keep);
    if (!anyTextured) {
        m_layerTable.reset();
        m_layerTableData.clear();
    } else if (table != m_layerTableData) {
        m_layerTable = Texture::create2D(r, 3, uint32_t(layers.size()), VK_FORMAT_R32G32B32A32_SFLOAT, table.data(),
                                         table.size() * sizeof(float), false, SamplerType::NearestClamp, "TerrainLayers");
        m_layerTableData = std::move(table);
    }

    m_levelMaterials.resize(size_t(m_lod->levels()));
    for (int level = 0; level < m_lod->levels(); ++level) {
        auto& m = m_levelMaterials[size_t(level)];
        if (!m) m = r.createMaterial(m_shader);
        m->setTexture(0, detail);
        m->setTexture(1, m_layerTable);
        m->setVector(0, {detailScales, 0.0f, 0.0f});
        m->setVector(1, {rockColor, float(wetnessSlot)});
        m->setVector(2, {waterLevel, float(biomes.size()), 0.0f, 0.0f});
        const glm::vec2 morph = m_lod->morphRange(level);
        m->setVector(3, {morph, 0.0f, 0.0f});
    }
}

void Terrain::updateNodes(bool preload)
{
    if (!m_lod) return;
    glm::vec3 eye = transform().position();
    if (Camera* cam = scene().mainCamera()) eye = cam->transform().position();
    m_lod->update(eye, preload, engine().loading()); // behind a loading screen: no upload budget

    // The drawn nodes go to pooled MeshRenderers on a child object. Node
    // vertices are in world space, so the child keeps an identity transform.
    GameObject* nodes = m_nodeObject.get(scene());
    if (!nodes) { // first time, or something deleted the child (and its renderers with it)
        nodes = &scene().spawn("TerrainNodes");
        nodes->hideInHierarchy = true;
        nodes->setParent(this, false);
        m_nodeObject = nodes;
        m_nodeRenderers.clear();
    }
    const glm::mat4& world = nodes->transform().worldMatrix();
    if (world != glm::mat4(1.0f)) {
        nodes->setParent(nullptr, false); // the terrain moved: back to the origin
        nodes->transform().setLocalPosition(glm::vec3(0.0f));
        nodes->transform().setLocalRotation(glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
        nodes->transform().setLocalScale(glm::vec3(1.0f));
        nodes->setParent(this, true);
    }
    const auto& drawn = m_lod->drawn();
    while (m_nodeRenderers.size() < drawn.size()) m_nodeRenderers.push_back(&nodes->addComponent<MeshRenderer>());
    for (size_t i = 0; i < m_nodeRenderers.size(); ++i) {
        MeshRenderer& mr = *m_nodeRenderers[i];
        if (i < drawn.size()) {
            mr.mesh = drawn[i].mesh;
            mr.materials = {m_levelMaterials[size_t(std::min<int>(drawn[i].level, int(m_levelMaterials.size()) - 1))]};
        } else {
            mr.mesh = nullptr;
            mr.materials.clear();
        }
    }
}

void Terrain::updateColliders(bool force, const glm::vec4* area)
{
    m_sinceColliders = 0.0f;
    const auto d = data();
    const glm::vec3 centre = transform().position();
    if (force) {
        for (auto& [key, rb] : m_colliderTiles) removeComponent(rb);
        m_colliderTiles.clear();
    }
    if (area) {
        // Brush edits: replace just the tiles under the brushed area.
        const float ts = float(TerrainData::kTileCells) * d->cellSize;
        for (auto it = m_colliderTiles.begin(); it != m_colliderTiles.end();) {
            const glm::vec2 lo = d->origin + glm::vec2(terrainTileCoords(it->first)) * ts;
            if (lo.x <= area->z && lo.x + ts >= area->x && lo.y <= area->w && lo.y + ts >= area->y) {
                removeComponent(it->second);
                it = m_colliderTiles.erase(it);
            } else {
                ++it;
            }
        }
    }
    const int cells = TerrainData::kTileCells;
    const float tileSize = float(cells) * d->cellSize;
    const int tiles = (d->res - 2) / cells + 1;
    auto tileOf = [&](glm::vec2 p) { return glm::ivec2(glm::floor((p - d->origin) / tileSize)); };

    // Ground is needed under the camera, every character and every dynamic body.
    // A pile of crates is hundreds of points: merge them per tile into a box
    // (its distance to a tile is <= each point's, so nothing needed is missed).
    struct Focus {
        glm::vec2 lo, hi;
    };
    std::unordered_map<int64_t, Focus> focusByTile;
    auto addFocus = [&](const glm::vec3& p3) {
        const glm::vec2 p(p3.x, p3.z);
        const glm::ivec2 t = tileOf(p);
        auto [it, fresh] = focusByTile.try_emplace(terrainTileKey(t.x, t.y), Focus{p, p});
        if (!fresh) it->second = {glm::min(it->second.lo, p), glm::max(it->second.hi, p)};
    };
    if (Camera* cam = scene().mainCamera()) addFocus(cam->transform().position());
    scene().forEach<CharacterController>([&](CharacterController& c) {
        if (c.isActive()) addFocus(c.transform().position());
    });
    scene().forEach<Rigidbody>([&](Rigidbody& rb) {
        if (rb.isActive() && rb.type == BodyType::Dynamic) addFocus(rb.transform().position());
    });
    std::vector<Focus> focus;
    focus.reserve(focusByTile.size());
    for (const auto& [key, f] : focusByTile) focus.push_back(f);

    auto distToTile = [&](const Focus& f, int tx, int tz) {
        const glm::vec2 lo = d->origin + glm::vec2(float(tx), float(tz)) * tileSize;
        const glm::vec2 q = glm::max(glm::max(lo - f.hi, f.lo - (lo + tileSize)), glm::vec2(0.0f));
        return glm::length(q);
    };
    const float keep = colliderDistance * 1.25f; // hysteresis
    // Drop tiles nothing is near any more.
    for (auto it = m_colliderTiles.begin(); it != m_colliderTiles.end();) {
        const glm::ivec2 t = terrainTileCoords(it->first);
        bool needed = false;
        for (const Focus& f : focus) needed = needed || distToTile(f, t.x, t.y) < keep;
        if (needed) {
            ++it;
        } else {
            removeComponent(it->second);
            it = m_colliderTiles.erase(it);
        }
    }
    // Missing tiles within range, nearest first.
    const int reach = int(std::ceil(colliderDistance / tileSize)) + 1;
    std::vector<std::pair<float, int64_t>> wanted;
    for (const Focus& f : focus) {
        const glm::ivec2 c0 = tileOf(f.lo), c1 = tileOf(f.hi);
        for (int tz = std::max(0, c0.y - reach); tz <= std::min(tiles - 1, c1.y + reach); ++tz) {
            for (int tx = std::max(0, c0.x - reach); tx <= std::min(tiles - 1, c1.x + reach); ++tx) {
                const int64_t key = terrainTileKey(tx, tz);
                const float dist = distToTile(f, tx, tz);
                if (m_colliderTiles.count(key) || dist >= colliderDistance) continue;
                wanted.push_back({dist, key});
            }
        }
    }
    std::sort(wanted.begin(), wanted.end());
    // Building a heightfield body is costly: a few per call (this runs every
    // 0.1 s), except tiles something stands on, which can't wait.
    constexpr int kMaxNewTiles = 4;
    int created = 0;
    for (const auto& [dist, key] : wanted) {
        if (m_colliderTiles.count(key)) continue; // wanted by two focus boxes
        if (dist > 0.0f && created >= kMaxNewTiles) break;
        const glm::ivec2 t = terrainTileCoords(key);
        // 65 x 65 samples (heights relative to this object, which the body follows).
        const int n = cells + 1;
        std::vector<float> rel(size_t(n) * n);
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) rel[size_t(j) * n + i] = d->sampleHeight(t.x * cells + i, t.y * cells + j) - centre.y;
        const glm::vec2 lo = d->samplePosition(t.x * cells, t.y * cells);
        auto& rb = addComponent<Rigidbody>(Collider::heightfield(rel, uint32_t(n), d->cellSize, {lo.x - centre.x, 0.0f, lo.y - centre.z}),
                                           BodyType::Static);
        rb.surface = "Terrain"; // (games look up the biome at the point for grass / sand / mud)
        m_colliderTiles[key] = &rb;
        ++created;
    }
}

Json Terrain::biomesJson() const
{
    Json a = Json::array();
    for (const auto& b : biomes) a.push_back(b.asset.empty() ? biomeToJson(b) : Json{{"asset", b.asset}});
    return a;
}

Json biomeToJson(const TerrainBiome& b)
{
    Json j = Json::object();
    if (const TypeInfo* t = TypeRegistry::get().find("TerrainBiome")) j = SceneSerializer::serializeFields(*t, &b);
    if (b.material.textured()) j["material"] = Terrain::materialJson(b.material);
    auto plants = [](const std::vector<BiomePlant>& list) {
        Json a = Json::array();
        for (const BiomePlant& p : list) a.push_back({{"type", p.type}, {"density", p.density}});
        return a;
    };
    if (!b.foliage.empty()) j["foliage"] = plants(b.foliage);
    if (!b.grass.empty()) j["grass"] = plants(b.grass);
    return j;
}

TerrainBiome biomeFromJson(const Json& j, const std::string& fallbackName)
{
    TerrainBiome b;
    b.name = fallbackName;
    if (!j.is_object()) return b;
    if (const TypeInfo* t = TypeRegistry::get().find("TerrainBiome")) SceneSerializer::applyFields(*t, &b, j);
    if (!j.contains("color2")) b.color2 = b.color; // one colour: no colour noise
    b.material = Terrain::materialFromJson(j.value("material", Json()));
    auto plants = [](const Json& a) {
        std::vector<BiomePlant> out;
        if (!a.is_array()) return out;
        for (const Json& e : a) {
            BiomePlant p;
            if (e.is_string()) {
                p.type = e.get<std::string>();
            } else if (e.is_object()) {
                p.type = e.value("type", std::string());
                p.density = e.value("density", 1.0f);
            }
            if (!p.type.empty()) out.push_back(std::move(p));
        }
        return out;
    };
    b.foliage = plants(j.value("foliage", Json()));
    b.grass = plants(j.value("grass", Json()));
    return b;
}

TerrainBiome loadBiomeAsset(const std::string& path)
{
    TerrainBiome b = biomeFromJson(*DataAssets::get(path), std::filesystem::path(path).stem().string());
    b.asset = path;
    return b;
}

void Terrain::setBiomesJson(const Json& j)
{
    if (!j.is_array()) return;
    biomes.clear();
    for (const Json& e : j) {
        if (e.is_object() && e.contains("asset") && e["asset"].is_string()) biomes.push_back(loadBiomeAsset(e["asset"].get<std::string>()));
        else biomes.push_back(biomeFromJson(e));
    }
    if (biomes.empty()) biomes.push_back(TerrainBiome{});
    m_biomeAssetsGeneration = DataAssets::generation();
}

void Terrain::refreshBiomeAssets()
{
    const uint64_t gen = DataAssets::generation();
    if (gen == m_biomeAssetsGeneration) return;
    m_biomeAssetsGeneration = gen;
    for (TerrainBiome& b : biomes) {
        if (b.asset.empty()) continue;
        TerrainBiome fresh = loadBiomeAsset(b.asset);
        if (biomeToJson(fresh) != biomeToJson(b)) {
            b = std::move(fresh);
            m_dirty = true; // (the scene-file state only holds the path: rebuild explicitly)
        }
    }
}

Json Terrain::materialJson(const TerrainMaterial& m)
{
    Json j = Json::object();
    if (!m.textured()) return j;
    for (auto [key, path] : {std::pair{"albedo", &m.albedo}, {"normal", &m.normal}, {"roughness", &m.roughness}, {"ao", &m.ao}})
        if (!path->empty()) j[key] = *path;
    j["size"] = m.size;
    j["tint"] = Json{m.tint.x, m.tint.y, m.tint.z};
    j["normalStrength"] = m.normalStrength;
    j["roughnessValue"] = m.roughnessValue;
    j["biomeColor"] = m.biomeColor;
    return j;
}

TerrainMaterial Terrain::materialFromJson(const Json& j)
{
    TerrainMaterial m;
    if (!j.is_object()) return m;
    m.albedo = j.value("albedo", m.albedo);
    m.normal = j.value("normal", m.normal);
    m.roughness = j.value("roughness", m.roughness);
    m.ao = j.value("ao", m.ao);
    m.size = j.value("size", m.size);
    m.tint = vec3From(j.value("tint", Json()), m.tint);
    m.normalStrength = j.value("normalStrength", m.normalStrength);
    m.roughnessValue = j.value("roughnessValue", m.roughnessValue);
    m.biomeColor = j.value("biomeColor", m.biomeColor);
    return m;
}

// -----------------------------------------------------------------------------
// Reflection
// -----------------------------------------------------------------------------

void registerTerrainTypes(TypeRegistry& r)
{
    // Plain data (biome files and the editor's biome panel). The JSON keys are
    // the scene files' biome keys (see biomeToJson / biomeFromJson).
    r.add<TerrainBiome>("TerrainBiome", [](TypeBuilder<TerrainBiome>& t) {
        t.category("World");
        t.field("name", &TerrainBiome::name).tooltip("what vegetation and stamps call it; unique per terrain");
        t.field("color", &TerrainBiome::color).color();
        t.field("color2", &TerrainBiome::color2).color().tooltip("mixed in by noise (colorNoiseFrequency)");
        t.field("colorNoiseFrequency", &TerrainBiome::colorNoiseFrequency).range(0.0f, 1.0f).speed(0.0005f);
        t.field("colorNoiseSeed", &TerrainBiome::colorNoiseSeed);
        t.field("patchColor", &TerrainBiome::patchColor).color();
        t.field("patchFrequency", &TerrainBiome::patchFrequency).range(0.0f, 1.0f).speed(0.0005f).tooltip("0 = no patches");
        t.field("patchSeed", &TerrainBiome::patchSeed);
        t.field("patchCoverage", &TerrainBiome::patchCoverage).range(0.0f, 1.0f).speed(0.01f);
        t.field("patchStrength", &TerrainBiome::patchStrength).range(0.0f, 1.0f).speed(0.01f);
        t.field("minHeight", &TerrainBiome::minHeight).speed(0.1f).tooltip("occurs on its own between these heights...");
        t.field("maxHeight", &TerrainBiome::maxHeight).speed(0.1f);
        t.field("maxSlope", &TerrainBiome::maxSlope).range(0.0f, 1.0f).speed(0.005f).tooltip("...up to this slope (1 - normal.y)...");
        t.field("coverage", &TerrainBiome::coverage).range(0.0f, 1.0f).speed(0.005f)
            .tooltip("...over about this fraction of that area (noise patches). 0 = only where stamps / the brush paint it. "
                     "Ignored for the first biome, which fills whatever the others leave");
        t.field("coverageFrequency", &TerrainBiome::coverageFrequency).range(0.0f, 1.0f).speed(0.0001f)
            .tooltip("size of the patches (1/m): smaller = bigger regions");
        t.field("coverageSeed", &TerrainBiome::coverageSeed);
    });
    r.add<TerrainMaterial>("TerrainMaterial", [](TypeBuilder<TerrainMaterial>& t) {
        t.category("World");
        t.field("albedo", &TerrainMaterial::albedo).assets(".png .jpg .jpeg .tga");
        t.field("normal", &TerrainMaterial::normal).assets(".png .jpg .jpeg .tga");
        t.field("roughness", &TerrainMaterial::roughness).assets(".png .jpg .jpeg .tga");
        t.field("ao", &TerrainMaterial::ao).assets(".png .jpg .jpeg .tga");
        t.field("size", &TerrainMaterial::size).range(0.05f, 1000.0f).speed(0.05f).tooltip("metres per texture repeat");
        t.field("tint", &TerrainMaterial::tint).color();
        t.field("normalStrength", &TerrainMaterial::normalStrength).range(0.0f, 4.0f).speed(0.01f);
        t.field("roughnessValue", &TerrainMaterial::roughnessValue).range(0.0f, 1.0f).speed(0.01f).tooltip("without a roughness map");
        t.field("biomeColor", &TerrainMaterial::biomeColor).range(0.0f, 1.0f).speed(0.01f)
            .tooltip("0 = the texture's colours, 1 = the biome's colour carrying the texture's detail");
    });
    r.add<Terrain>("Terrain", [](TypeBuilder<Terrain>& t) {
        t.category("World");
        t.field("seed", &Terrain::seed);
        t.field("size", &Terrain::size).range(16.0f, 16384.0f).speed(1.0f);
        t.field("cellSize", &Terrain::cellSize).range(0.25f, 16.0f).speed(0.05f);
        t.field("chunkCells", &Terrain::chunkCells).range(4.0f, 128.0f);
        t.field("tileCacheMB", &Terrain::tileCacheMB).range(1.0f, 4096.0f).tooltip("sample tiles kept in memory");
        t.field("lodFactor", &Terrain::lodFactor).range(1.1f, 8.0f).speed(0.01f).tooltip("higher = finer ground further away");
        t.field("colliderDistance", &Terrain::colliderDistance).range(32.0f, 4096.0f)
            .tooltip("physics tiles around the camera, characters and dynamic bodies");
        t.field("baseHeight", &Terrain::baseHeight);
        t.field("noiseFrequency", &Terrain::noiseFrequency).range(0.0f, 0.1f).speed(0.0001f);
        t.field("noiseAmplitude", &Terrain::noiseAmplitude).range(0.0f, 500.0f);
        t.field("noiseOctaves", &Terrain::noiseOctaves).range(1.0f, 8.0f);
        t.field("waves", &Terrain::waves).tooltip("amplitude, x frequency, z frequency");
        t.field("borderStart", &Terrain::borderStart).range(0.0f, 10000.0f);
        t.field("borderEnd", &Terrain::borderEnd).range(0.0f, 10000.0f);
        t.field("borderHeight", &Terrain::borderHeight).range(0.0f, 1000.0f);
        t.field("borderNoise", &Terrain::borderNoise).range(0.0f, 1000.0f);
        t.field("borderNoiseFrequency", &Terrain::borderNoiseFrequency).range(0.0f, 0.1f).speed(0.0001f);
        t.field("waterLevel", &Terrain::waterLevel);
        t.field("rockColor", &Terrain::rockColor).color();
        t.field("mudColor", &Terrain::mudColor).color();
        t.field("steepStart", &Terrain::steepStart).range(0.0f, 1.0f).speed(0.005f);
        t.field("steepEnd", &Terrain::steepEnd).range(0.0f, 1.0f).speed(0.005f);
        t.field("borderRockStart", &Terrain::borderRockStart);
        t.field("borderRockEnd", &Terrain::borderRockEnd);
        t.field("borderRock", &Terrain::borderRock).range(0.0f, 1.0f).speed(0.01f);
        t.field("brightnessVariation", &Terrain::brightnessVariation).range(0.0f, 1.0f).speed(0.01f);
        t.property("edits", FieldKind::Json, [](const Terrain& x) { return x.editsJson(); },
                   [](Terrain& x, const Json& j) { x.setEditsJson(j); })
            .hidden();
        t.property("rockMaterial", FieldKind::Json, [](const Terrain& x) { return Terrain::materialJson(x.rockMaterial); },
                   [](Terrain& x, const Json& j) { x.rockMaterial = Terrain::materialFromJson(j); })
            .tooltip("optional rock texture (projected from three sides): {albedo, normal, roughness, ao, size, tint, "
                     "normalStrength, roughnessValue, biomeColor}");
        t.field("detailTexture", &Terrain::detailTexture).assets(".png .jpg .jpeg .tga");
        t.field("detailScales", &Terrain::detailScales);
        t.field("wetnessSlot", &Terrain::wetnessSlot).range(-1.0f, 7.0f);
        t.property("biomes", FieldKind::Json, [](const Terrain& x) { return x.biomesJson(); },
                   [](Terrain& x, const Json& j) { x.setBiomesJson(j); })
            .tooltip("[{asset: 'biomes/x.biome'} or an inline biome {name, color, ...}]; edit them in the Biomes "
                     "section below. Biome 0 fills the rest, later biomes paint over earlier ones")
            .hidden(); // the editor draws its own biome list (drawTerrainTools)
    });
    r.add<TerrainStamp>("TerrainStamp", [](TypeBuilder<TerrainStamp>& t) {
        t.category("World");
        t.enumField("mode", &TerrainStamp::mode, {"Flatten (to this height)", "Raise"});
        t.field("radius", &TerrainStamp::radius).range(0.0f, 10000.0f);
        t.field("falloff", &TerrainStamp::falloff).range(0.0f, 10000.0f);
        t.field("noiseAmplitude", &TerrainStamp::noiseAmplitude).tooltip("Flatten: noisy pools / bumps added to the target height");
        t.field("noiseFrequency", &TerrainStamp::noiseFrequency).range(0.0f, 1.0f).speed(0.001f);
        t.field("noiseSeed", &TerrainStamp::noiseSeed);
        t.field("height", &TerrainStamp::height).tooltip("Raise: peak height (negative = pit)");
        t.field("exponent", &TerrainStamp::exponent).range(0.1f, 8.0f).speed(0.01f);
        t.field("ridgeAmount", &TerrainStamp::ridgeAmount);
        t.field("ridgeFrequency", &TerrainStamp::ridgeFrequency).range(0.0f, 1.0f).speed(0.001f);
        t.field("ridgeSeed", &TerrainStamp::ridgeSeed);
        t.field("biome", &TerrainStamp::biome).tooltip("Paint this biome (by name) around the stamp");
        t.field("biomeRadius", &TerrainStamp::biomeRadius).range(0.0f, 10000.0f);
        t.field("biomeFalloff", &TerrainStamp::biomeFalloff).range(0.0f, 10000.0f);
        t.field("biomeStrength", &TerrainStamp::biomeStrength).range(0.0f, 1.0f).speed(0.01f);
    });
}

} // namespace ve
