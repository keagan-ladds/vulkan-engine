#include "Terrain.hpp"

#include "Noise2D.hpp"

#include <ve/assets/AssetManager.hpp>
#include <ve/core/Log.hpp>
#include <ve/render/Mesh.hpp>
#include <ve/render/Renderer.hpp>
#include <ve/render/Texture.hpp>
#include <ve/scene/Components.hpp>
#include <ve/scene/Scene.hpp>

#include <algorithm>

namespace savanna {

namespace {
float smooth(float e0, float e1, float x)
{
    return glm::smoothstep(e0, e1, x);
}
} // namespace

float Terrain::heightFunction(float x, float z)
{
    const glm::vec2 p(x, z);

    // Rolling savannah plains.
    float h = 2.5f + (fbm(p * 0.0035f, 5, 1) - 0.5f) * 16.0f + std::sin(x * 0.011f) * std::cos(z * 0.013f) * 1.5f;

    // Swamp basin: sink towards water level with hummocks and pools.
    const float swamp = 1.0f - smooth(55.0f, 140.0f, glm::length(p - kSwamp));
    const float pools = -1.3f + fbm(p * 0.025f, 3, 7) * 3.2f;
    h = glm::mix(h, pools, swamp);

    // Kopje: granite domes rising out of the plain.
    const float k = 1.0f - smooth(0.0f, 48.0f, glm::length(p - kKopje));
    const float ridges = 1.0f - std::abs(fbm(p * 0.03f, 3, 11) * 2.0f - 1.0f);
    h += std::pow(k, 1.6f) * 24.0f + k * ridges * 5.0f;
    const float k2 = 1.0f - smooth(0.0f, 26.0f, glm::length(p - glm::vec2(-50.0f, -140.0f)));
    h += std::pow(k2, 1.4f) * 13.0f;

    // Flatten the camp.
    const float camp = 1.0f - smooth(14.0f, 34.0f, glm::length(p - kCamp));
    h = glm::mix(h, 3.2f, camp);

    // Mountains along the border hide the edge of the world.
    const float edge = smooth(200.0f, 315.0f, std::max(std::abs(x), std::abs(z)));
    h += edge * edge * (55.0f + fbm(p * 0.009f, 3, 23) * 60.0f);
    return h;
}

float Terrain::swampMask(float x, float z) const
{
    return 1.0f - smooth(55.0f, 140.0f, glm::length(glm::vec2(x, z) - kSwamp));
}

float Terrain::rockMask(float x, float z) const
{
    const glm::vec2 p(x, z);
    const float k = 1.0f - smooth(20.0f, 50.0f, glm::length(p - kKopje));
    const float k2 = 1.0f - smooth(8.0f, 28.0f, glm::length(p - glm::vec2(-50.0f, -140.0f)));
    const float steep = smooth(0.25f, 0.45f, slope(x, z));
    return std::max({k, k2, steep});
}

bool Terrain::inBounds(float x, float z, float margin) const
{
    return std::abs(x) < kHalfSize - margin && std::abs(z) < kHalfSize - margin;
}

Terrain::Terrain() : GameObject("Terrain")
{
    m_res = static_cast<int>(2.0f * kHalfSize / kCellSize) + 1;
    m_heights.resize(size_t(m_res) * m_res);
    for (int j = 0; j < m_res; ++j) {
        for (int i = 0; i < m_res; ++i) {
            m_heights[size_t(j) * m_res + i] = heightFunction(-kHalfSize + i * kCellSize, -kHalfSize + j * kCellSize);
        }
    }
}

void Terrain::start()
{
    buildMeshes();
}

float Terrain::height(float x, float z) const
{
    const float fx = std::clamp((x + kHalfSize) / kCellSize, 0.0f, float(m_res - 1) - 0.001f);
    const float fz = std::clamp((z + kHalfSize) / kCellSize, 0.0f, float(m_res - 1) - 0.001f);
    const int ix = static_cast<int>(fx), iz = static_cast<int>(fz);
    const float tx = fx - ix, tz = fz - iz;
    auto at = [&](int i, int j) { return m_heights[size_t(j) * m_res + i]; };
    // Same triangulation as the mesh (split along the i,j+1 -> i+1,j diagonal).
    if (tx + tz < 1.0f) {
        return at(ix, iz) + (at(ix + 1, iz) - at(ix, iz)) * tx + (at(ix, iz + 1) - at(ix, iz)) * tz;
    }
    return at(ix + 1, iz + 1) + (at(ix, iz + 1) - at(ix + 1, iz + 1)) * (1.0f - tx) +
           (at(ix + 1, iz) - at(ix + 1, iz + 1)) * (1.0f - tz);
}

glm::vec3 Terrain::normal(float x, float z) const
{
    const float e = kCellSize;
    const float hl = height(x - e, z), hr = height(x + e, z);
    const float hd = height(x, z - e), hu = height(x, z + e);
    return glm::normalize(glm::vec3(hl - hr, 2.0f * e, hd - hu));
}

glm::vec4 Terrain::colorAt(float x, float z, float h, const glm::vec3& n) const
{
    const glm::vec2 p(x, z);
    const glm::vec3 dryGrass(0.72f, 0.56f, 0.29f);
    const glm::vec3 greenGrass(0.45f, 0.46f, 0.21f);
    const glm::vec3 redSoil(0.58f, 0.31f, 0.17f);
    const glm::vec3 swampGreen(0.20f, 0.25f, 0.10f);
    const glm::vec3 mud(0.23f, 0.18f, 0.12f);
    const glm::vec3 rock(0.50f, 0.46f, 0.41f);

    glm::vec3 c = glm::mix(dryGrass, greenGrass, smooth(0.45f, 0.7f, fbm(p * 0.012f, 3, 31)));
    c = glm::mix(c, redSoil, smooth(0.6f, 0.7f, fbm(p * 0.02f + 3.0f, 3, 41)) * 0.85f);
    c *= 0.9f + 0.2f * fbm(p * 0.15f, 2, 51);
    c = glm::mix(c, swampGreen, swampMask(x, z));
    c = glm::mix(c, mud, smooth(1.0f, -0.2f, h));
    const float camp = 1.0f - smooth(8.0f, 26.0f, glm::length(p - kCamp));
    c = glm::mix(c, redSoil * 0.9f, camp * 0.8f);
    const float steep = smooth(0.3f, 0.5f, 1.0f - n.y);
    const float edge = smooth(230.0f, 300.0f, std::max(std::abs(x), std::abs(z)));
    c = glm::mix(c, rock, std::max(steep, std::max(rockMask(x, z) * 0.9f, edge * 0.6f)));
    return glm::vec4(c, 1.0f);
}

void Terrain::buildMeshes()
{
    ve::Renderer& r = renderer();

    // Custom terrain material: vertex-coloured biomes + a tiling detail texture.
    ve::ShaderDesc desc;
    desc.name = "Terrain";
    desc.vertex = "terrain.vert";
    desc.fragment = "terrain.frag";
    m_material = r.createMaterial(r.createShader(desc));
    m_material->setTexture(0, assets().loadTexture("textures/ground_detail.png", false));
    m_material->setVector(0, {0.31f, 0.043f, 0.0f, 0.0f}); // detail UV scales
    m_material->setVector(1, {0.50f, 0.46f, 0.41f, 0.0f}); // rock colour

    const int chunks = (m_res - 1) / kChunkCells;
    for (int cz = 0; cz < chunks; ++cz) {
        for (int cx = 0; cx < chunks; ++cx) {
            ve::MeshData md;
            const int n = kChunkCells + 1;
            for (int j = 0; j < n; ++j) {
                for (int i = 0; i < n; ++i) {
                    const int gi = cx * kChunkCells + i, gj = cz * kChunkCells + j;
                    const float x = -kHalfSize + gi * kCellSize, z = -kHalfSize + gj * kCellSize;
                    ve::Vertex v;
                    v.position = {x, m_heights[size_t(gj) * m_res + gi], z};
                    v.normal = normal(x, z);
                    v.uv = {x, z};
                    v.color = colorAt(x, z, v.position.y, v.normal);
                    md.vertices.push_back(v);
                }
            }
            for (int j = 0; j < kChunkCells; ++j) {
                for (int i = 0; i < kChunkCells; ++i) {
                    const uint32_t a = j * n + i;
                    md.indices.insert(md.indices.end(), {a, a + n, a + 1, a + 1, a + n, a + n + 1});
                }
            }
            auto mesh = ve::Mesh::create(r, md, {}, "terrain chunk");
            auto& chunk = scene().spawn("TerrainChunk");
            chunk.addComponent<ve::MeshRenderer>(mesh, m_material);
            chunk.setParent(this, false);
        }
    }
    ve::log::info("Terrain: {}x{} samples, {} chunks", m_res, m_res, chunks * chunks);
}

} // namespace savanna
