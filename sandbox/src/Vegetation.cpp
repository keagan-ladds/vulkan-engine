#include "Vegetation.hpp"

#include "Noise2D.hpp"
#include "Terrain.hpp"

#include <ve/core/Engine.hpp>
#include <ve/core/Log.hpp>
#include <ve/render/Mesh.hpp>
#include <ve/render/Primitives.hpp>
#include <ve/physics/Physics.hpp>
#include <ve/render/Impostor.hpp>
#include <ve/render/Instancing.hpp>
#include <ve/render/Renderer.hpp>
#include <ve/world/Foliage.hpp>
#include <ve/world/Grass.hpp>
#include <ve/render/Texture.hpp>

#include <cstdlib>
#include <ve/render/Renderer.hpp>
#include <ve/scene/Components.hpp>
#include <ve/scene/Scene.hpp>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <iterator>

#include <unordered_map>

namespace savanna {

using ve::MeshData;
namespace prim = ve::primitives;

namespace {

glm::mat4 trs(const glm::vec3& t, float yaw, float tilt, const glm::vec3& s = glm::vec3(1.0f))
{
    glm::mat4 m = glm::translate(glm::mat4(1.0f), t);
    m = glm::rotate(m, yaw, glm::vec3(0, 1, 0));
    m = glm::rotate(m, tilt, glm::vec3(1, 0, 0)); // lean towards -Z after yaw... any direction works
    return glm::scale(m, s);
}

// Organic blob: icosphere with noise displacement.
MeshData blob(Rng& rng, float roughness, uint32_t subdiv = 1)
{
    MeshData m = prim::icosphere(1.0f, subdiv);
    const float seed = rng.range(0.0f, 100.0f);
    for (auto& v : m.vertices) {
        const glm::vec3 p = v.position;
        const float n = valueNoise(glm::vec2(p.x * 1.7f + seed, p.z * 1.7f + p.y * 1.3f), 3);
        v.position = p * (1.0f - roughness * 0.5f + roughness * n);
    }
    return m;
}

glm::vec4 vary(Rng& rng, const glm::vec3& c, float amount)
{
    return glm::vec4(c * rng.range(1.0f - amount, 1.0f + amount), 1.0f);
}

MeshData acacia(uint32_t seed)
{
    Rng rng(seed);
    MeshData m;
    const glm::vec3 bark(0.30f, 0.24f, 0.19f), leaves(0.22f, 0.30f, 0.10f);
    const float trunkH = rng.range(2.6f, 3.6f);
    const float lean = rng.range(-0.18f, 0.18f);
    const float leanYaw = rng.range(0.0f, glm::two_pi<float>());
    const glm::mat4 trunkT = trs(glm::vec3(0.0f), leanYaw, lean);
    m.append(prim::cylinder(0.32f, 0.2f, trunkH, 7, false), trunkT, vary(rng, bark, 0.1f));
    const glm::vec3 top = glm::vec3(trunkT * glm::vec4(0, trunkH, 0, 1));

    // Branches spreading up and out, each ending in a flat leaf cluster.
    const int branches = 3 + static_cast<int>(rng.next() * 2.0f);
    for (int i = 0; i < branches; ++i) {
        const float yaw = (static_cast<float>(i) + rng.range(-0.3f, 0.3f)) * glm::two_pi<float>() / branches;
        const float tilt = rng.range(0.55f, 0.95f);
        const float len = rng.range(1.8f, 2.8f);
        const glm::mat4 bt = trs(top, yaw, tilt);
        m.append(prim::cylinder(0.14f, 0.07f, len, 5, false), bt, vary(rng, bark, 0.1f));
        const glm::vec3 tip = glm::vec3(bt * glm::vec4(0, len, 0, 1));
        const float r = rng.range(1.8f, 2.8f);
        m.append(blob(rng, 0.5f), trs(tip + glm::vec3(0, 0.25f, 0), rng.range(0.0f, 6.28f), 0.0f,
                                      {r, rng.range(0.4f, 0.55f), r * rng.range(0.8f, 1.2f)}),
                 vary(rng, leaves, 0.15f));
    }
    // The iconic wide, flat crown.
    const float crown = rng.range(3.4f, 4.6f);
    m.append(blob(rng, 0.4f), trs(top + glm::vec3(0, 1.7f, 0), rng.range(0.0f, 6.28f), 0.0f, {crown, 0.65f, crown * 0.9f}),
             vary(rng, leaves, 0.1f));
    m.makeFlatShaded();
    return m;
}

MeshData baobab(uint32_t seed)
{
    Rng rng(seed);
    MeshData m;
    const glm::vec3 bark(0.56f, 0.48f, 0.44f), leaves(0.30f, 0.38f, 0.13f);
    const float h = rng.range(6.0f, 7.5f);
    m.append(prim::cylinder(1.6f, 1.15f, h, 10, true), glm::mat4(1.0f), vary(rng, bark, 0.06f));
    const int branches = 6;
    for (int i = 0; i < branches; ++i) {
        const float yaw = (static_cast<float>(i) + rng.range(-0.2f, 0.2f)) * glm::two_pi<float>() / branches;
        const float len = rng.range(1.6f, 2.8f);
        const glm::mat4 bt = trs(glm::vec3(0, h - 0.3f, 0) + glm::vec3(std::cos(yaw), 0, std::sin(yaw)) * 0.6f, yaw,
                                 rng.range(0.6f, 1.1f));
        m.append(prim::cylinder(0.38f, 0.12f, len, 6, false), bt, vary(rng, bark, 0.06f));
        if (rng.next() < 0.6f) {
            const glm::vec3 tip = glm::vec3(bt * glm::vec4(0, len, 0, 1));
            m.append(blob(rng, 0.5f), trs(tip, 0.0f, 0.0f, {1.0f, 0.55f, 1.0f}), vary(rng, leaves, 0.15f));
        }
    }
    m.makeFlatShaded();
    return m;
}

MeshData feverTree(uint32_t seed)
{
    Rng rng(seed);
    MeshData m;
    const glm::vec3 bark(0.68f, 0.68f, 0.34f), leaves(0.29f, 0.38f, 0.14f);
    const float h = rng.range(5.5f, 8.0f);
    const glm::mat4 tt = trs(glm::vec3(0.0f), rng.range(0.0f, 6.28f), rng.range(-0.1f, 0.1f));
    m.append(prim::cylinder(0.22f, 0.12f, h, 6, false), tt, vary(rng, bark, 0.08f));
    const glm::vec3 top = glm::vec3(tt * glm::vec4(0, h, 0, 1));
    for (int i = 0; i < 4; ++i) {
        const float yaw = rng.range(0.0f, 6.28f);
        const float len = rng.range(1.5f, 2.6f);
        const glm::vec3 base = glm::vec3(tt * glm::vec4(0, h * rng.range(0.6f, 0.9f), 0, 1));
        const glm::mat4 bt = trs(base, yaw, rng.range(0.4f, 0.7f));
        m.append(prim::cylinder(0.09f, 0.04f, len, 5, false), bt, vary(rng, bark, 0.08f));
        const glm::vec3 tip = glm::vec3(bt * glm::vec4(0, len, 0, 1));
        const float r = rng.range(1.2f, 1.9f);
        m.append(blob(rng, 0.5f), trs(tip, rng.range(0.0f, 6.28f), 0.0f, {r, 0.45f, r}), vary(rng, leaves, 0.15f));
    }
    m.append(blob(rng, 0.5f), trs(top, 0.0f, 0.0f, {2.0f, 0.6f, 2.0f}), vary(rng, leaves, 0.1f));
    m.makeFlatShaded();
    return m;
}

MeshData bush(uint32_t seed, bool dry)
{
    Rng rng(seed);
    MeshData m;
    const glm::vec3 c = dry ? glm::vec3(0.45f, 0.40f, 0.21f) : glm::vec3(0.27f, 0.33f, 0.13f);
    for (int i = 0; i < 3; ++i) {
        const glm::vec3 off(rng.range(-0.7f, 0.7f), rng.range(0.3f, 0.6f), rng.range(-0.7f, 0.7f));
        const float r = rng.range(0.7f, 1.2f);
        m.append(blob(rng, 0.6f), trs(off, rng.range(0.0f, 6.28f), 0.0f, {r, r * rng.range(0.6f, 0.85f), r}),
                 vary(rng, c, 0.12f));
    }
    m.makeFlatShaded();
    return m;
}

MeshData rock(uint32_t seed, float flatness)
{
    Rng rng(seed);
    MeshData m;
    m.append(blob(rng, 0.7f, 2), trs({0, 0.25f, 0}, 0.0f, 0.0f, {rng.range(1.0f, 1.5f), flatness, rng.range(0.8f, 1.2f)}),
             vary(rng, glm::vec3(0.49f, 0.45f, 0.40f), 0.1f));
    m.makeFlatShaded();
    return m;
}

} // namespace

PropLibrary buildPropLibrary(ve::Renderer& r)
{
    PropLibrary lib;
    auto mat = r.createStandardMaterial();
    mat->setBaseColor(glm::vec4(1.0f)).setRoughness(0.85f).setVertexColorWeight(1.0f);
    lib.material = mat;
    // Each variant gets a LOD chain: the generated mesh, simplified levels
    // (meshoptimizer; faceted keeps the low-poly flat shading) switched by
    // screen size, and for trees an octahedral impostor as the last level.
    using SO = ve::SimplifyOptions;
    auto addVariant = [&](PropSet& set, const MeshData& md, const char* name, std::vector<std::pair<SO, float>> levels,
                          float lod0ScreenSize, bool impostor) {
        MeshData lod0 = md;
        lod0.optimize();
        auto mesh0 = ve::Mesh::create(r, lod0, {}, name);
        std::vector<ve::InstanceLod> chain{{mesh0, {lib.material}, lod0ScreenSize}};
        std::string tris = std::to_string(lod0.triangleCount());
        for (const auto& [options, screenSize] : levels) {
            MeshData lod = md.simplified(options);
            lod.optimize();
            tris += " / " + std::to_string(lod.triangleCount());
            chain.push_back({ve::Mesh::create(r, lod, {}, name), {lib.material}, screenSize});
        }
        if (impostor) {
            chain.push_back(ve::Impostor::bake(r, *mesh0, {lib.material})->lod());
            tris += " / impostor";
        }
        if (set.variants.empty()) ve::log::info("LODs {}: {} triangles", name, tris);
        set.variants.push_back(std::move(chain));
    };
    // Screen size = fraction of half the screen height the bounding radius covers.
    const SO treeMid{.ratio = 0.35f, .maxError = 0.08f, .faceted = true};
    const SO smallFar{.ratio = 0.25f, .maxError = 0.3f, .sloppy = true, .faceted = true};
    for (uint32_t i = 0; i < 5; ++i) addVariant(lib.acacias, acacia(100 + i), "acacia", {{treeMid, 0.035f}}, 0.10f, true);
    for (uint32_t i = 0; i < 2; ++i) addVariant(lib.baobabs, baobab(200 + i), "baobab", {{treeMid, 0.035f}}, 0.10f, true);
    for (uint32_t i = 0; i < 3; ++i) addVariant(lib.feverTrees, feverTree(300 + i), "fever tree", {{treeMid, 0.035f}}, 0.10f, true);
    for (uint32_t i = 0; i < 4; ++i) addVariant(lib.bushes, bush(400 + i, i % 2 == 0), "bush", {{smallFar, 0.0f}}, 0.08f, false);
    // Rocks: the collider is the convex hull of the rock's own (full detail) mesh.
    for (uint32_t i = 0; i < 4; ++i) {
        const MeshData md = rock(500 + i, 0.75f);
        addVariant(lib.rocks, md, "rock", {{smallFar, 0.0f}}, 0.06f, false);
        lib.rocks.colliders.push_back(ve::Collider::convexHull(md));
    }
    for (uint32_t i = 0; i < 3; ++i) {
        const MeshData md = rock(600 + i, 1.1f);
        addVariant(lib.boulders, md, "boulder", {{SO{.ratio = 0.3f, .maxError = 0.1f, .faceted = true}, 0.0f}}, 0.06f, false);
        lib.boulders.colliders.push_back(ve::Collider::convexHull(md));
    }
    // Trees: only the trunk is solid (shapes stand on y = 0, like the meshes).
    lib.acacias.colliders.push_back(ve::Collider::capsule(1.5f, 0.3f).offset({0.0f, 1.5f, 0.0f}));
    lib.baobabs.colliders.push_back(ve::Collider::cylinder(3.3f, 1.45f).offset({0.0f, 3.3f, 0.0f}));
    lib.feverTrees.colliders.push_back(ve::Collider::capsule(3.0f, 0.2f).offset({0.0f, 3.0f, 0.0f}));
    // Bushes have no collider: you can walk through them.
    return lib;
}

namespace {
// Names for raycast hits on instanced props (RaycastHit::userValue).
const char* const kPropNames[] = {"", "baobab", "acacia", "fever tree", "boulder", "rock", "bush"};
} // namespace

const char* propName(uint32_t userValue)
{
    return userValue < std::size(kPropNames) ? kPropNames[userValue] : "";
}

ve::FoliageSystem& scatterProps(ve::Scene& scene, const Terrain& terrain, const PropLibrary& lib)
{
    // Streamed, rule-based placement (ve::FoliageSystem): every layer is a
    // jittered grid filtered by biome rules, so the same world appears no
    // matter where the player starts or in which order cells load.
    ve::FoliageWorld world;
    const float edge = Terrain::kHalfSize - 45.0f; // stay off the border mountains
    world.boundsMin = glm::vec2(-edge);
    world.boundsMax = glm::vec2(edge);
    world.height = [&terrain](glm::vec2 p) { return terrain.height(p.x, p.y); };
    world.normal = [&terrain](glm::vec2 p) { return terrain.normal(p.x, p.y); };
    auto& foliage = scene.spawn<ve::FoliageSystem>(world);
    foliage.addExclusion(Terrain::kCamp, 26.0f);

    auto layer = [&](uint32_t kind, const PropSet& set, float spacing, float probability, float minScale, float maxScale) {
        ve::FoliageLayer l;
        l.name = propName(kind);
        l.variants = set.variants;
        l.colliders = set.colliders;
        l.colliderUserValue = kind;
        l.spacing = spacing;
        l.probability = probability;
        l.minScale = minScale;
        l.maxScale = maxScale;
        l.tint = 0.13f;
        l.sink = 0.15f;
        l.seed = 1000u + kind;
        return l;
    };
    auto savannah = [&terrain](const ve::FoliageSample& s) {
        return s.height > 0.6f && terrain.swampMask(s.position.x, s.position.y) < 0.3f &&
                       terrain.rockMask(s.position.x, s.position.y) < 0.4f
                   ? 1.0f
                   : 0.0f;
    };

    // Trees: the whole (small) world stays loaded; impostors make that cheap.
    auto baobabs = layer(1, lib.baobabs, 30.0f, 0.3f, 0.9f, 1.3f);
    baobabs.density = savannah;
    baobabs.viewDistance = 3000.0f;
    baobabs.maxInstances = 300;
    const uint32_t baobabLayer = foliage.addLayer(baobabs);

    auto acacias = layer(2, lib.acacias, 8.5f, 0.82f, 0.8f, 1.3f);
    acacias.density = savannah;
    acacias.clusterFrequency = 0.012f; // groves
    acacias.clusterCoverage = 0.5f;
    acacias.avoid = {{baobabLayer, 12.0f}};
    acacias.viewDistance = 3000.0f;
    acacias.maxInstances = 5000;
    const uint32_t acaciaLayer = foliage.addLayer(acacias);

    auto fever = layer(3, lib.feverTrees, 5.5f, 0.6f, 0.8f, 1.2f);
    fever.minHeight = 0.2f;
    fever.density = [&terrain](const ve::FoliageSample& s) {
        return terrain.swampMask(s.position.x, s.position.y) > 0.35f ? 1.0f : 0.0f;
    };
    fever.viewDistance = 3000.0f;
    fever.maxInstances = 2000;
    const uint32_t feverLayer = foliage.addLayer(fever);

    auto boulders = layer(4, lib.boulders, 6.0f, 0.35f, 2.0f, 5.0f);
    boulders.minHeight = 0.5f;
    boulders.alignToNormal = 0.5f;
    boulders.density = [&terrain](const ve::FoliageSample& s) {
        const glm::vec2 p = s.position;
        const bool nearKopje = glm::length(p - Terrain::kKopje) < 55.0f || glm::length(p - glm::vec2(-50.0f, -140.0f)) < 30.0f;
        return nearKopje && terrain.rockMask(p.x, p.y) > 0.5f ? 1.0f : 0.0f;
    };
    boulders.viewDistance = 3000.0f;
    boulders.maxInstances = 500;
    const uint32_t boulderLayer = foliage.addLayer(boulders);

    // Small props stream in around the camera; colliders only near the player.
    auto rocks = layer(5, lib.rocks, 6.0f, 0.18f, 0.4f, 1.4f);
    rocks.minHeight = 0.3f;
    rocks.alignToNormal = 0.7f;
    rocks.avoid = {{boulderLayer, 3.0f}};
    rocks.viewDistance = 450.0f;
    rocks.shadowDistance = 50.0f;
    rocks.colliderDistance = 40.0f;
    foliage.addLayer(rocks);

    auto bushes = layer(6, lib.bushes, 4.5f, 0.5f, 0.7f, 1.4f);
    bushes.minHeight = 0.3f;
    bushes.avoid = {{baobabLayer, 5.0f}, {acaciaLayer, 2.5f}, {feverLayer, 1.5f}, {boulderLayer, 3.0f}};
    bushes.viewDistance = 350.0f;
    bushes.shadowDistance = 60.0f;
    bushes.colliderDistance = 40.0f;
    foliage.addLayer(bushes);
    return foliage;
}

namespace {
// A grass-card texture: tapered, slightly curved blades on transparent
// background (stand-in for an artist-made card).
std::shared_ptr<ve::Texture> makeGrassCardTexture(ve::Renderer& r)
{
    constexpr int W = 128, H = 256;
    std::vector<uint8_t> px(size_t(W) * H * 4, 0);
    Rng rng(99);
    for (int b = 0; b < 14; ++b) {
        const float x0 = rng.range(10.0f, W - 10.0f);
        const float lean = rng.range(-30.0f, 30.0f);
        const float top = rng.range(0.05f, 0.45f) * H; // blade tip row (image top = card top)
        const float width = rng.range(3.0f, 6.5f);
        const glm::vec3 base = glm::mix(glm::vec3(0.55f, 0.48f, 0.25f), glm::vec3(0.72f, 0.7f, 0.42f), rng.next());
        for (int y = int(top); y < H; ++y) {
            const float t = (float(H) - float(y)) / (float(H) - top); // 0 root .. 1 tip
            const float cx = x0 + lean * t * t;
            const float hw = width * (1.0f - t) + 0.4f;
            for (int x = int(cx - hw - 1); x <= int(cx + hw + 1); ++x) {
                if (x < 0 || x >= W) continue;
                const float cover = glm::clamp(hw - std::abs(float(x) - cx) + 0.5f, 0.0f, 1.0f);
                if (cover <= 0.0f) continue;
                uint8_t* p = &px[(size_t(y) * W + x) * 4];
                const glm::vec3 c = base * glm::mix(0.6f, 1.15f, t);
                p[0] = uint8_t(255.0f * glm::clamp(c.r, 0.0f, 1.0f));
                p[1] = uint8_t(255.0f * glm::clamp(c.g, 0.0f, 1.0f));
                p[2] = uint8_t(255.0f * glm::clamp(c.b, 0.0f, 1.0f));
                p[3] = std::max(p[3], uint8_t(255.0f * cover));
            }
        }
    }
    return ve::Texture::create2D(r, W, H, VK_FORMAT_R8G8B8A8_SRGB, px.data(), px.size(), true,
                                 ve::SamplerType::LinearClamp, "grass card");
}
} // namespace

void addGrass(ve::Renderer& r, const Terrain& terrain)
{
    // GPU grass (ve::GrassPass): nothing stored per blade; density, colour and
    // height come from masks sampled once from the terrain's biome functions.
    ve::GrassDesc grass;
    grass.name = "Grass";
    grass.boundsMin = glm::vec2(-Terrain::kHalfSize);
    grass.boundsMax = glm::vec2(Terrain::kHalfSize);
    grass.resolution = uint32_t(2.0f * Terrain::kHalfSize / Terrain::kCellSize) + 1; // = the terrain's samples
    grass.maskResolution = 640;
    grass.height = [&terrain](glm::vec2 p) { return terrain.height(p.x, p.y); };
    grass.mask = [&terrain](glm::vec2 p) {
        const float h = terrain.height(p.x, p.y);
        if (h < 0.25f || terrain.rockMask(p.x, p.y) > 0.45f || terrain.swampMask(p.x, p.y) > 0.6f) return glm::vec4(0.0f);
        const float camp = glm::smoothstep(8.0f, 14.0f, glm::length(p - Terrain::kCamp)); // trampled camp floor
        const float density = glm::clamp(fbm(p * 0.03f, 2, 61) * 2.0f - 0.2f, 0.0f, 1.0f) * camp;
        return glm::vec4(density, fbm(p * 0.012f, 3, 31), glm::mix(0.7f, 1.0f, fbm(p * 0.05f, 2, 13)), 0.0f);
    };
    grass.colorA = {0.78f, 0.63f, 0.32f}; // dry
    grass.colorB = {0.55f, 0.55f, 0.26f}; // greener
    grass.spacing = 0.75f;
    grass.bladesPerClump = 6;
    grass.clumpRadius = 0.3f;
    grass.minHeight = 0.45f;
    grass.maxHeight = 1.05f;
    grass.bladeWidth = 0.06f;
    grass.fragmentShader = "grass.frag"; // engine placement + wind, savanna look (wetness, translucency)
    if (std::getenv("SAVANNA_GRASSCARDS")) {
        // Alpha-clipped cards instead of procedural blades.
        ve::GrassDesc cards = grass;
        cards.cardMesh = ve::GrassDesc::cardMeshCrossed(3, 1.1f);
        cards.cardTexture = makeGrassCardTexture(r);
        cards.colorA = glm::vec3(1.05f, 0.95f, 0.8f); // tints multiply the texture
        cards.colorB = glm::vec3(0.85f, 0.95f, 0.75f);
        cards.spacing = 1.1f;
        cards.minHeight = 0.5f;
        cards.maxHeight = 1.0f;
        cards.maxTilt = 0.25f;
        cards.fragmentShader = ""; // engine grass_card.frag
        r.graph().addPass<ve::GrassPass>(cards);
    } else {
        r.graph().addPass<ve::GrassPass>(grass);
    }

    ve::GrassDesc reeds = grass;
    reeds.name = "Reeds";
    reeds.mask = [&terrain](glm::vec2 p) {
        const float h = terrain.height(p.x, p.y);
        const bool reed = terrain.swampMask(p.x, p.y) > 0.4f && h > -0.5f && h < 0.6f;
        return reed ? glm::vec4(0.9f, fbm(p * 0.05f, 2, 3), 1.0f, 0.0f) : glm::vec4(0.0f);
    };
    reeds.colorA = {0.30f, 0.40f, 0.14f};
    reeds.colorB = {0.42f, 0.46f, 0.18f};
    reeds.spacing = 1.2f;
    reeds.bladesPerClump = 5;
    reeds.minHeight = 1.4f;
    reeds.maxHeight = 2.3f;
    reeds.bladeWidth = 0.03f;
    reeds.clumpRadius = 0.3f;
    reeds.radius = 140.0f;
    r.graph().addPass<ve::GrassPass>(reeds);
}

} // namespace savanna
