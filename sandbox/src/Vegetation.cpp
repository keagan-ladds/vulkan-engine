#include "Vegetation.hpp"

#include "Noise2D.hpp"
#include "Props.hpp"

#include <ve/core/Log.hpp>
#include <ve/physics/Physics.hpp>
#include <ve/render/Impostor.hpp>
#include <ve/render/Instancing.hpp>
#include <ve/render/Mesh.hpp>
#include <ve/render/Primitives.hpp>
#include <ve/render/Renderer.hpp>
#include <ve/world/Vegetation.hpp>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <iterator>
#include <string>

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

namespace {
// Each variant gets a LOD chain: the generated mesh, simplified levels
// (meshoptimizer; faceted keeps the low-poly flat shading) switched by screen
// size (fraction of half the screen height the bounding radius covers), and
// for trees an octahedral impostor as the last level.
using SO = ve::SimplifyOptions;
void addVariant(ve::Renderer& r, ve::FoliageMeshes& set, const MeshData& md, const char* name,
                std::vector<std::pair<SO, float>> levels, float lod0ScreenSize, bool impostor)
{
    const auto material = propMaterial();
    MeshData lod0 = md;
    lod0.optimize();
    auto mesh0 = ve::Mesh::create(r, lod0, {}, name);
    std::vector<ve::InstanceLod> chain{{mesh0, {material}, lod0ScreenSize}};
    std::string tris = std::to_string(lod0.triangleCount());
    for (const auto& [options, screenSize] : levels) {
        MeshData lod = md.simplified(options);
        lod.optimize();
        tris += " / " + std::to_string(lod.triangleCount());
        chain.push_back({ve::Mesh::create(r, lod, {}, name), {material}, screenSize});
    }
    if (impostor) {
        chain.push_back(ve::Impostor::bake(r, *mesh0, {material})->lod());
        tris += " / impostor";
    }
    if (set.variants.empty()) ve::log::info("LODs {}: {} triangles", name, tris);
    set.variants.push_back(std::move(chain));
}

const SO kTreeMid{.ratio = 0.35f, .maxError = 0.08f, .faceted = true};
const SO kSmallFar{.ratio = 0.25f, .maxError = 0.3f, .sloppy = true, .faceted = true};

// Names for raycast hits on instanced props (RaycastHit::userValue).
const char* const kPropNames[] = {"", "baobab", "acacia", "fever tree", "boulder", "rock", "bush"};
} // namespace

const char* propName(uint32_t userValue)
{
    return userValue < std::size(kPropNames) ? kPropNames[userValue] : "";
}

void registerPropMeshes()
{
    ve::MeshLibrary::add("acacia", [](ve::Renderer& r) {
        ve::FoliageMeshes set;
        for (uint32_t i = 0; i < 5; ++i) addVariant(r, set, acacia(100 + i), "acacia", {{kTreeMid, 0.035f}}, 0.10f, true);
        // Trees: only the trunk is solid (shapes stand on y = 0, like the meshes).
        set.colliders.push_back(ve::Collider::capsule(1.5f, 0.3f).offset({0.0f, 1.5f, 0.0f}));
        set.colliderUserValue = 2;
        return set;
    });
    ve::MeshLibrary::add("baobab", [](ve::Renderer& r) {
        ve::FoliageMeshes set;
        for (uint32_t i = 0; i < 2; ++i) addVariant(r, set, baobab(200 + i), "baobab", {{kTreeMid, 0.035f}}, 0.10f, true);
        set.colliders.push_back(ve::Collider::cylinder(3.3f, 1.45f).offset({0.0f, 3.3f, 0.0f}));
        set.colliderUserValue = 1;
        return set;
    });
    ve::MeshLibrary::add("fever tree", [](ve::Renderer& r) {
        ve::FoliageMeshes set;
        for (uint32_t i = 0; i < 3; ++i) addVariant(r, set, feverTree(300 + i), "fever tree", {{kTreeMid, 0.035f}}, 0.10f, true);
        set.colliders.push_back(ve::Collider::capsule(3.0f, 0.2f).offset({0.0f, 3.0f, 0.0f}));
        set.colliderUserValue = 3;
        return set;
    });
    ve::MeshLibrary::add("bush", [](ve::Renderer& r) {
        ve::FoliageMeshes set; // no collider: you can walk through bushes
        for (uint32_t i = 0; i < 4; ++i) addVariant(r, set, bush(400 + i, i % 2 == 0), "bush", {{kSmallFar, 0.0f}}, 0.08f, false);
        set.colliderUserValue = 6;
        return set;
    });
    // Rocks: the collider is the convex hull of the rock's own (full detail) mesh.
    ve::MeshLibrary::add("rock", [](ve::Renderer& r) {
        ve::FoliageMeshes set;
        for (uint32_t i = 0; i < 4; ++i) {
            const MeshData md = rock(500 + i, 0.75f);
            addVariant(r, set, md, "rock", {{kSmallFar, 0.0f}}, 0.06f, false);
            set.colliders.push_back(ve::Collider::convexHull(md));
        }
        set.colliderUserValue = 5;
        return set;
    });
    ve::MeshLibrary::add("boulder", [](ve::Renderer& r) {
        ve::FoliageMeshes set;
        for (uint32_t i = 0; i < 3; ++i) {
            const MeshData md = rock(600 + i, 1.1f);
            addVariant(r, set, md, "boulder", {{SO{.ratio = 0.3f, .maxError = 0.1f, .faceted = true}, 0.0f}}, 0.06f, false);
            set.colliders.push_back(ve::Collider::convexHull(md));
        }
        set.colliderUserValue = 4;
        return set;
    });
}

} // namespace savanna
