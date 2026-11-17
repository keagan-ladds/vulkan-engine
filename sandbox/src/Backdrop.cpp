#include "Backdrop.hpp"

#include <ve/render/Material.hpp>
#include <ve/render/Mesh.hpp>
#include <ve/render/RenderGraph.hpp>
#include <ve/render/Renderer.hpp>
#include <ve/scene/Components.hpp>
#include <ve/scene/Scene.hpp>
#include <ve/world/Noise.hpp>

#include <algorithm>
#include <cmath>

namespace savanna {

namespace {

float smooth(float a, float b, float x)
{
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Ridged noise: sharp crests where plain noise crosses its middle.
float ridged(glm::vec2 p, int octaves, uint32_t seed)
{
    float v = 0.0f, amp = 0.5f, norm = 0.0f;
    for (int o = 0; o < octaves; ++o) {
        const float n = 1.0f - std::abs(ve::noise::valueNoise(p, seed + uint32_t(o) * 31u) * 2.0f - 1.0f);
        v += n * n * amp;
        norm += amp;
        amp *= 0.5f;
        p = p * 2.07f + glm::vec2(11.3f, 4.1f);
    }
    return v / norm;
}

} // namespace

MountainBackdrop::MountainBackdrop() : GameObject("Mountain backdrop")
{
    runInEditor = true;
    // Hero massifs, placed for the views from the camp: east catches the
    // setting sun on its cliffs, north is side-lit, west is a silhouette at sunset.
    massifs = {
        {{5200.0f, -1400.0f}, 1100.0f, 950.0f},
        {{7400.0f, 2600.0f}, 900.0f, 700.0f},
        {{-2400.0f, -6800.0f}, 1600.0f, 1250.0f},
        {{-6400.0f, 1200.0f}, 1000.0f, 780.0f},
    };
}

float MountainBackdrop::height(glm::vec2 p) const
{
    const float r = glm::length(p);
    // Foothills continuing the terrain's border hills, then the range.
    const float foothills = 60.0f + 120.0f * ve::noise::fbm(p / 700.0f, 4, 5u);
    float rise = smooth(innerRadius + 800.0f, innerRadius + 3200.0f, r) * (1.0f - smooth(outerRadius - 2500.0f, outerRadius, r));
    // Separate ranges, not a wall: large-scale breaks where it drops to low hills.
    rise *= 0.15f + 0.85f * smooth(0.38f, 0.62f, ve::noise::fbm(p / 7000.0f, 3, 77u));

    // Mesas: ridged noise with a steep step -> plateaus behind sheer walls.
    const glm::vec2 warp(ve::noise::fbm(p / 3000.0f, 3, 9u), ve::noise::fbm(p / 3000.0f + 7.3f, 3, 10u));
    const glm::vec2 q = p / 2400.0f + warp * 1.2f;
    const float m = 0.55f * ve::noise::fbm(q, 4, 21u) + 0.45f * ridged(q * 1.3f, 4, 33u);
    const float cliff = smooth(0.5f, 0.56f, m);
    float h = foothills + rise * (cliff * (550.0f + 450.0f * ve::noise::fbm(p / 900.0f, 4, 41u)) + m * 260.0f);

    // Hero massifs: sheer, slightly irregular walls with a domed, eroded top.
    for (const Massif& s : massifs) {
        const glm::vec2 d = p - s.center;
        const float edgeNoise = ve::noise::fbm(glm::normalize(d + 1e-3f) * 2.5f + s.center * 1e-3f, 3, 51u);
        const float dn = glm::length(d) / (s.radius * (0.8f + 0.45f * edgeNoise));
        const float wall = 1.0f - smooth(0.82f, 0.97f, dn);
        const float top = 1.0f - 0.35f * dn * dn + 0.12f * (ridged(p / 350.0f, 3, 61u) - 0.5f);
        const float apron = (1.0f - smooth(0.9f, 1.6f, dn)) * 0.18f; // scree at the foot
        h = std::max(h, foothills + s.height * std::max(wall * top, apron));
    }
    return h;
}

void MountainBackdrop::start()
{
    ve::Renderer& r = renderer();
    ve::ShaderDesc desc;
    desc.name = "mountain backdrop";
    desc.vertex = "standard.vert";
    desc.fragment = "backdrop.frag";
    desc.castShadows = false;
    m_material = r.createMaterial(r.createShader(desc));
    m_material->setVector(0, {hazeDensity, hazeFalloff, 0.85f, 0.0f});
    const ve::TextureId lut = r.graph().texture("SkyViewLUT");
    m_material->setTextureIndex(0, lut != ve::kNoTexture ? r.graph().sampled(lut) : ve::kInvalidIndex);

    // Polar grid: rings spaced geometrically (detail where it's closest), split
    // into angular sectors so the ones behind the camera are culled.
    constexpr int kSectors = 32, kSegmentsPerSector = 56, kRings = 120;
    const int segments = kSectors * kSegmentsPerSector;
    std::vector<float> ringR(kRings);
    for (int i = 0; i < kRings; ++i) ringR[size_t(i)] = innerRadius * std::pow(outerRadius / innerRadius, float(i) / float(kRings - 1));

    for (int sector = 0; sector < kSectors; ++sector) {
        ve::MeshData md;
        const int a0 = sector * kSegmentsPerSector;
        for (int i = 0; i < kRings; ++i) {
            for (int a = a0; a <= a0 + kSegmentsPerSector; ++a) {
                const float ang = float(a) / float(segments) * 6.2831853f;
                const glm::vec2 p = glm::vec2(std::cos(ang), std::sin(ang)) * ringR[size_t(i)];
                ve::Vertex v;
                v.position = {p.x, height(p), p.y};
                md.vertices.push_back(v);
            }
        }
        const int row = kSegmentsPerSector + 1;
        for (int i = 0; i + 1 < kRings; ++i) {
            for (int a = 0; a < kSegmentsPerSector; ++a) {
                const uint32_t v00 = uint32_t(i * row + a), v01 = v00 + 1, v10 = v00 + uint32_t(row), v11 = v10 + 1;
                // Counter-clockwise seen from above (outward ring = larger radius).
                md.indices.insert(md.indices.end(), {v00, v01, v10, v01, v11, v10});
            }
        }
        md.computeNormals();
        // Colour: sandstone with strata, redder on cliffs, scrub on gentle low slopes, paler crests.
        for (ve::Vertex& v : md.vertices) {
            const glm::vec2 p(v.position.x, v.position.z);
            const float slope = 1.0f - std::clamp(v.normal.y, 0.0f, 1.0f);
            const float n = ve::noise::fbm(p / 160.0f, 3, 71u);
            const float strata = 0.5f + 0.5f * std::sin(v.position.y * 0.055f + n * 4.0f);
            const glm::vec3 sand(0.58f, 0.42f, 0.30f), dark(0.36f, 0.25f, 0.19f), red(0.55f, 0.30f, 0.20f);
            glm::vec3 rock = glm::mix(sand, dark, strata * 0.55f);
            rock = glm::mix(rock, red, smooth(0.45f, 0.8f, slope) * 0.5f);
            const glm::vec3 scrub = glm::mix(glm::vec3(0.30f, 0.30f, 0.17f), glm::vec3(0.42f, 0.38f, 0.22f), n);
            float green = (1.0f - smooth(0.18f, 0.4f, slope)) * (1.0f - smooth(250.0f, 700.0f, v.position.y));
            glm::vec3 c = glm::mix(rock, scrub, green * 0.85f);
            c *= 0.85f + 0.3f * n;
            v.color = glm::vec4(c, 1.0f);
        }
        auto& part = scene().spawn("Mountains " + std::to_string(sector));
        part.setParent(this, false);
        part.hideInHierarchy = true;
        auto& mr = part.addComponent<ve::MeshRenderer>(ve::Mesh::create(r, md, {}, "mountains"), m_material);
        mr.castShadows = false;
    }
}

void MountainBackdrop::lateUpdate(float)
{
    // The peaks are up to ~14 km away: make sure whatever camera is active sees them.
    if (ve::Camera* cam = scene().mainCamera(); cam && cam->farPlane < outerRadius * 1.5f) cam->farPlane = outerRadius * 1.5f;
}

} // namespace savanna
