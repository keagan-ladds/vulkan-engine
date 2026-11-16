#include "ve/world/Terrain.hpp"

#include "ve/assets/AssetManager.hpp"
#include "ve/core/Log.hpp"
#include "ve/physics/Physics.hpp"
#include "ve/render/Material.hpp"
#include "ve/render/Mesh.hpp"
#include "ve/render/Renderer.hpp"
#include "ve/render/Texture.hpp"
#include "ve/scene/Components.hpp"
#include "ve/scene/Scene.hpp"
#include "ve/scene/SceneSerializer.hpp"
#include "ve/world/Noise.hpp"

#include <algorithm>
#include <atomic>
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

int64_t tileKey(int tx, int tz) { return (int64_t(tz) << 32) | int64_t(uint32_t(tx)); }

} // namespace

TerrainData::TerrainData(const TerrainShape& shape, glm::vec3 centre, std::vector<TerrainBiome> biomes,
                         std::vector<std::pair<glm::vec3, TerrainStampShape>> stamps, size_t cacheBytes)
    : m_shape(shape), m_centre(centre), m_biomes(std::move(biomes)), m_uid(g_terrainUid++)
{
    cellSize = std::max(shape.cellSize, 0.1f);
    res = std::max(2, int(std::round(shape.size / cellSize)) + 1);
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
            // Biome rules: height range, slope, noise coverage, then stamps paint over them.
            const glm::vec2 p = samplePosition(clampIdx(i0 + ii), clampIdx(j0 + jj));
            const glm::vec3 n = glm::normalize(glm::vec3(ah(ii - 1, jj) - ah(ii + 1, jj), 2.0f * e, ah(ii, jj - 1) - ah(ii, jj + 1)));
            const float slope = 1.0f - n.y;
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
                tile->biomes[b][idx] = v;
                remaining -= v;
            }
            tile->biomes[0][idx] = remaining;
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
    const int64_t key = tileKey(tx, tz);
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
    // Same triangulation as the mesh (split along the i,j+1 -> i+1,j diagonal).
    if (tx + tz < 1.0f) return at(ix, iz) + (at(ix + 1, iz) - at(ix, iz)) * tx + (at(ix, iz + 1) - at(ix, iz)) * tz;
    return at(ix + 1, iz + 1) + (at(ix, iz + 1) - at(ix + 1, iz + 1)) * (1.0f - tx) +
           (at(ix + 1, iz) - at(ix + 1, iz + 1)) * (1.0f - tz);
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
    if (const TypeInfo* self = TypeRegistry::get().find("Terrain")) j.push_back(SceneSerializer::serializeFields(*self, this));
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

std::shared_ptr<TerrainData> Terrain::generate() const
{
    // A snapshot of the inputs: samples are generated lazily, tile by tile.
    std::vector<std::pair<glm::vec3, TerrainStampShape>> stamps;
    scene().forEach<TerrainStamp>([&](TerrainStamp& s) {
        if (s.isActive()) stamps.push_back({s.transform().position(), s});
    });
    const size_t stampCount = stamps.size();
    auto d = std::make_shared<TerrainData>(*this, transform().position(), biomes, std::move(stamps),
                                           size_t(std::max(tileCacheMB, 1.0f) * 1024.0f * 1024.0f));
    log::info("Terrain: {:.0f} m, {}x{} samples in tiles of {} cells (generated on demand), {} biomes, {} stamps",
              float(d->res - 1) * d->cellSize, d->res, d->res, TerrainData::kTileCells, d->biomeNames.size(), stampCount);
    return d;
}

void Terrain::start()
{
    m_started = true;
    data();
    buildVisuals();
}

void Terrain::update(float dt)
{
    m_sinceRebuild += dt;
    m_sinceCheck += dt;
    // Serializing every field (stamps, biomes) is too costly for every frame;
    // edits are picked up within a quarter second (code can call markDirty()).
    if (m_sinceCheck >= 0.25f) {
        m_sinceCheck = 0.0f;
        if (editState().dump() != m_editState) m_dirty = true;
    }
    if (m_dirty && m_sinceRebuild > 0.25f) rebuild();
}

void Terrain::rebuild()
{
    m_dirty = false;
    m_sinceRebuild = 0.0f;
    auto fresh = generate();
    {
        std::lock_guard lock(m_mutex);
        m_data = fresh;
    }
    m_editState = editState().dump();
    ++m_version;
    if (m_started) buildVisuals();
}

void Terrain::onDestroy()
{
    for (GameObject* c : m_chunks) c->destroy();
    m_chunks.clear();
}

void Terrain::buildVisuals()
{
    Renderer& r = renderer();
    const auto d = data();
    if (!m_material) {
        ShaderDesc desc;
        desc.name = "Terrain";
        desc.vertex = "terrain.vert";
        desc.fragment = "terrain.frag";
        m_material = r.createMaterial(r.createShader(desc));
    }
    std::shared_ptr<Texture> detail;
    if (!detailTexture.empty()) {
        try {
            detail = assets().loadTexture(detailTexture, false);
        } catch (const std::exception& e) {
            log::warn("Terrain: {}", e.what());
        }
    }
    m_material->setTexture(0, detail);
    m_material->setVector(0, {detailScales, 0.0f, 0.0f});
    m_material->setVector(1, {rockColor, float(wetnessSlot)});
    m_material->setVector(2, {waterLevel, 0.0f, 0.0f, 0.0f});

    // Vertex colours: blended biome colours, mud near water, rock on steep slopes / the border.
    const int res = d->res;
    const glm::vec3 centre = transform().position();
    std::vector<glm::vec3> biomeColorScratch(biomes.size());
    auto colorAt = [&](const glm::vec2& p, float h, const glm::vec3& n, int gi, int gj) {
        glm::vec3 c(0.0f);
        for (size_t b = 0; b < biomes.size(); ++b) {
            const float w = d->sampleBiome(int(b), gi, gj);
            if (w <= 0.0f) continue;
            const TerrainBiome& bio = biomes[b];
            glm::vec3 bc = bio.color;
            if (bio.color2 != bio.color && bio.colorNoiseFrequency > 0.0f)
                bc = glm::mix(bio.color, bio.color2, smooth(0.45f, 0.7f, noise::fbm(p * bio.colorNoiseFrequency, 3, static_cast<uint32_t>(bio.colorNoiseSeed))));
            if (bio.patchFrequency > 0.0f) {
                const float n2 = noise::fbm(p * bio.patchFrequency + 3.0f, 3, static_cast<uint32_t>(bio.patchSeed));
                bc = glm::mix(bc, bio.patchColor, smooth(1.0f - bio.patchCoverage - 0.05f, 1.0f - bio.patchCoverage + 0.05f, n2) * bio.patchStrength);
            }
            c += bc * w;
        }
        if (biomes.empty()) c = glm::vec3(0.5f);
        c *= 1.0f - brightnessVariation * 0.5f + brightnessVariation * noise::fbm(p * 0.15f, 2, 51);
        c = glm::mix(c, mudColor, smooth(waterLevel + 1.0f, waterLevel - 0.2f, h));
        const float steep = smooth(steepStart, steepEnd, 1.0f - n.y);
        const float edge = smooth(borderRockStart, borderRockEnd, std::max(std::abs(p.x - centre.x), std::abs(p.y - centre.z)));
        c = glm::mix(c, rockColor, std::max(steep, edge * borderRock));
        return glm::vec4(c, 1.0f);
    };

    for (GameObject* c : m_chunks) c->destroy();
    m_chunks.clear();
    const int cells = std::max(chunkCells, 4);
    const int chunks = std::max(1, (res - 1 + cells - 1) / cells);
    for (int cz = 0; cz < chunks; ++cz) {
        for (int cx = 0; cx < chunks; ++cx) {
            const int i0 = cx * cells, j0 = cz * cells;
            const int ni = std::min(cells, res - 1 - i0) + 1, nj = std::min(cells, res - 1 - j0) + 1;
            if (ni < 2 || nj < 2) continue;
            MeshData md;
            md.vertices.reserve(size_t(ni) * size_t(nj));
            for (int j = 0; j < nj; ++j) {
                for (int i = 0; i < ni; ++i) {
                    const int gi = i0 + i, gj = j0 + j;
                    const glm::vec2 p = d->origin + glm::vec2(float(gi), float(gj)) * d->cellSize;
                    Vertex v;
                    v.position = {p.x, d->sampleHeight(gi, gj), p.y};
                    v.normal = d->normal(p.x, p.y);
                    v.uv = p;
                    v.color = colorAt(p, v.position.y, v.normal, gi, gj);
                    md.vertices.push_back(v);
                }
            }
            for (int j = 0; j < nj - 1; ++j) {
                for (int i = 0; i < ni - 1; ++i) {
                    const auto a = static_cast<uint32_t>(j * ni + i);
                    const auto n = static_cast<uint32_t>(ni);
                    md.indices.insert(md.indices.end(), {a, a + n, a + 1, a + 1, a + n, a + n + 1});
                }
            }
            auto& chunk = scene().spawn("TerrainChunk");
            chunk.hideInHierarchy = true;
            chunk.addComponent<MeshRenderer>(Mesh::create(r, md, {}, "terrain chunk"), m_material);
            chunk.setParent(this, true); // world-space vertices: keep the identity world transform
            m_chunks.push_back(&chunk);
        }
    }

    // Physics: the same samples as a heightfield (relative to this object's position).
    if (m_collider) removeComponent(m_collider);
    std::vector<float> rel(size_t(res) * size_t(res));
    for (int j = 0; j < res; ++j)
        for (int i = 0; i < res; ++i) rel[size_t(j) * size_t(res) + size_t(i)] = d->sampleHeight(i, j) - centre.y;
    auto& rb = addComponent<Rigidbody>(Collider::heightfield(rel, static_cast<uint32_t>(res), d->cellSize,
                                                             {d->origin.x - centre.x, 0.0f, d->origin.y - centre.z}),
                                       BodyType::Static);
    m_collider = &rb;
}

Json Terrain::biomesJson() const
{
    auto v3 = [](const glm::vec3& v) { return Json{v.x, v.y, v.z}; };
    Json a = Json::array();
    for (const auto& b : biomes) {
        a.push_back({{"name", b.name}, {"color", v3(b.color)}, {"color2", v3(b.color2)},
                     {"colorNoiseFrequency", b.colorNoiseFrequency}, {"colorNoiseSeed", b.colorNoiseSeed},
                     {"patchColor", v3(b.patchColor)}, {"patchFrequency", b.patchFrequency}, {"patchSeed", b.patchSeed},
                     {"patchCoverage", b.patchCoverage}, {"patchStrength", b.patchStrength},
                     {"minHeight", b.minHeight}, {"maxHeight", b.maxHeight}, {"maxSlope", b.maxSlope},
                     {"coverage", b.coverage}, {"coverageFrequency", b.coverageFrequency}, {"coverageSeed", b.coverageSeed}});
    }
    return a;
}

void Terrain::setBiomesJson(const Json& j)
{
    if (!j.is_array()) return;
    biomes.clear();
    for (const Json& e : j) {
        TerrainBiome b;
        b.name = e.value("name", b.name);
        b.color = vec3From(e.value("color", Json()), b.color);
        b.color2 = vec3From(e.value("color2", Json()), b.color);
        b.colorNoiseFrequency = e.value("colorNoiseFrequency", b.colorNoiseFrequency);
        b.colorNoiseSeed = e.value("colorNoiseSeed", b.colorNoiseSeed);
        b.patchColor = vec3From(e.value("patchColor", Json()), b.patchColor);
        b.patchFrequency = e.value("patchFrequency", b.patchFrequency);
        b.patchSeed = e.value("patchSeed", b.patchSeed);
        b.patchCoverage = e.value("patchCoverage", b.patchCoverage);
        b.patchStrength = e.value("patchStrength", b.patchStrength);
        b.minHeight = e.value("minHeight", b.minHeight);
        b.maxHeight = e.value("maxHeight", b.maxHeight);
        b.maxSlope = e.value("maxSlope", b.maxSlope);
        b.coverage = e.value("coverage", b.coverage);
        b.coverageFrequency = e.value("coverageFrequency", b.coverageFrequency);
        b.coverageSeed = e.value("coverageSeed", b.coverageSeed);
        biomes.push_back(b);
    }
    if (biomes.empty()) biomes.push_back(TerrainBiome{});
}

// -----------------------------------------------------------------------------
// Reflection
// -----------------------------------------------------------------------------

void registerTerrainTypes(TypeRegistry& r)
{
    r.add<Terrain>("Terrain", [](TypeBuilder<Terrain>& t) {
        t.category("World");
        t.field("seed", &Terrain::seed);
        t.field("size", &Terrain::size).range(16.0f, 16384.0f).speed(1.0f);
        t.field("cellSize", &Terrain::cellSize).range(0.25f, 16.0f).speed(0.05f);
        t.field("chunkCells", &Terrain::chunkCells).range(4.0f, 128.0f);
        t.field("tileCacheMB", &Terrain::tileCacheMB).range(1.0f, 4096.0f).tooltip("sample tiles kept in memory");
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
        t.field("detailTexture", &Terrain::detailTexture).assets(".png .jpg .jpeg .tga");
        t.field("detailScales", &Terrain::detailScales);
        t.field("wetnessSlot", &Terrain::wetnessSlot).range(-1.0f, 7.0f);
        t.property("biomes", FieldKind::Json, [](const Terrain& x) { return x.biomesJson(); },
                   [](Terrain& x, const Json& j) { x.setBiomesJson(j); })
            .tooltip("[{name, color, color2, patchColor, patchFrequency, minHeight, maxHeight, maxSlope, coverage, ...}]; "
                     "biome 0 fills the rest, later biomes paint over earlier ones");
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
