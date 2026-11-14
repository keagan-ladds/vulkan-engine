#include "Vegetation.hpp"

#include "Noise2D.hpp"
#include "Terrain.hpp"

#include <ve/core/Engine.hpp>
#include <ve/core/Log.hpp>
#include <ve/render/Mesh.hpp>
#include <ve/render/Primitives.hpp>
#include <ve/physics/Physics.hpp>
#include <ve/render/Instancing.hpp>
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
    // Each variant: LOD 0 = the generated mesh, then simplified levels
    // (meshoptimizer). Faceted = keep the low-poly flat shading on every LOD.
    using SO = ve::SimplifyOptions;
    auto addVariant = [&](PropSet& set, const MeshData& md, const char* name, std::initializer_list<SO> lodOptions) {
        MeshData lod0 = md;
        lod0.optimize();
        set.meshes.push_back(ve::Mesh::create(r, lod0, {}, name));
        std::vector<std::shared_ptr<ve::Mesh>> extra;
        std::string tris = std::to_string(lod0.triangleCount());
        for (const SO& o : lodOptions) {
            MeshData lod = md.simplified(o);
            lod.optimize();
            tris += " / " + std::to_string(lod.triangleCount());
            extra.push_back(ve::Mesh::create(r, lod, {}, name));
        }
        set.lods.push_back(std::move(extra));
        if (set.meshes.size() == 1) ve::log::info("LODs {}: {} triangles", name, tris);
    };
    const SO treeMid{.ratio = 0.35f, .maxError = 0.08f, .faceted = true};
    const SO treeFar{.ratio = 0.10f, .maxError = 0.3f, .sloppy = true, .faceted = true};
    const SO smallFar{.ratio = 0.25f, .maxError = 0.3f, .sloppy = true, .faceted = true};
    for (uint32_t i = 0; i < 5; ++i) addVariant(lib.acacias, acacia(100 + i), "acacia", {treeMid, treeFar});
    for (uint32_t i = 0; i < 2; ++i) addVariant(lib.baobabs, baobab(200 + i), "baobab", {treeMid, treeFar});
    for (uint32_t i = 0; i < 3; ++i) addVariant(lib.feverTrees, feverTree(300 + i), "fever tree", {treeMid, treeFar});
    for (uint32_t i = 0; i < 4; ++i) addVariant(lib.bushes, bush(400 + i, i % 2 == 0), "bush", {smallFar});
    // Rocks: the collider is the convex hull of the rock's own (full detail) mesh.
    for (uint32_t i = 0; i < 4; ++i) {
        const MeshData md = rock(500 + i, 0.75f);
        addVariant(lib.rocks, md, "rock", {smallFar});
        lib.rocks.colliders.push_back(ve::Collider::convexHull(md));
    }
    for (uint32_t i = 0; i < 3; ++i) {
        const MeshData md = rock(600 + i, 1.1f);
        addVariant(lib.boulders, md, "boulder", {SO{.ratio = 0.3f, .maxError = 0.1f, .faceted = true}});
        lib.boulders.colliders.push_back(ve::Collider::convexHull(md));
    }
    // LOD switch points: fraction of half the screen height covered by the
    // instance's bounding radius (so bigger/closer things keep detail longer).
    lib.acacias.screenSizes = lib.baobabs.screenSizes = lib.feverTrees.screenSizes = {0.10f, 0.035f};
    lib.bushes.screenSizes = {0.08f};
    lib.rocks.screenSizes = {0.06f};
    lib.boulders.screenSizes = {0.06f};
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

PropField::~PropField()
{
    for (uint32_t id : bodies) physics().destroyStaticBody(id);
}

void scatterProps(ve::Scene& scene, const Terrain& terrain, const PropLibrary& lib)
{
    // All props are GPU instances: one InstanceGroup per mesh variant, no
    // GameObject per tree. Colliders are GameObject-less static bodies.
    auto& field = scene.spawn<PropField>();
    Rng rng(42);

    // Spatial hash so spacing checks stay O(1) with tens of thousands of props.
    struct Placed {
        glm::vec2 p;
        float r;
    };
    constexpr float kCell = 20.0f;
    std::unordered_map<int64_t, std::vector<Placed>> grid;
    auto key = [](int x, int z) { return (int64_t(x) << 32) ^ int64_t(uint32_t(z)); };
    auto free = [&](glm::vec2 p, float r) {
        const int cx = int(std::floor(p.x / kCell)), cz = int(std::floor(p.y / kCell));
        const int reach = int(std::ceil((r + 20.0f) / kCell)); // 20 m = largest radius used
        for (int z = cz - reach; z <= cz + reach; ++z) {
            for (int x = cx - reach; x <= cx + reach; ++x) {
                auto it = grid.find(key(x, z));
                if (it == grid.end()) continue;
                for (const auto& o : it->second) {
                    if (glm::length(o.p - p) < o.r + r) return false;
                }
            }
        }
        return true;
    };

    auto place = [&](uint32_t kind, const PropSet& set, int count, float spacing, float minScale, float maxScale,
                     float cullDistance, float shadowDistance, auto&& accept) {
        // One instance group per mesh variant.
        std::vector<std::shared_ptr<ve::InstanceGroup>> groups;
        const uint32_t perVariant = uint32_t(count) / uint32_t(set.meshes.size()) + 64;
        for (size_t v = 0; v < set.meshes.size(); ++v) {
            ve::InstanceGroupDesc desc;
            desc.name = propName(kind);
            // LOD chain: full mesh, then the simplified levels, switched by screen size.
            desc.lods.push_back({set.meshes[v], {lib.material}, set.screenSizes.empty() ? 0.0f : set.screenSizes[0]});
            for (size_t l = 0; l < set.lods[v].size(); ++l) {
                const float next = l + 1 < set.screenSizes.size() ? set.screenSizes[l + 1] : 0.0f;
                desc.lods.push_back({set.lods[v][l], {lib.material}, next});
            }
            desc.cullDistance = cullDistance;
            desc.shadowDistance = shadowDistance;
            groups.push_back(ve::Engine::get().renderer().createInstanceGroup(desc, perVariant));
        }
        int done = 0;
        for (int attempt = 0; attempt < count * 30 && done < count; ++attempt) {
            const glm::vec2 p(rng.range(-Terrain::kHalfSize + 45.0f, Terrain::kHalfSize - 45.0f),
                              rng.range(-Terrain::kHalfSize + 45.0f, Terrain::kHalfSize - 45.0f));
            const float h = terrain.height(p.x, p.y);
            if (glm::length(p - Terrain::kCamp) < 26.0f || !accept(p, h) || !free(p, spacing * 0.5f)) continue;
            const size_t variant = size_t(rng.next() * set.meshes.size()) % set.meshes.size();
            if (groups[variant]->count() >= groups[variant]->capacity()) continue;
            grid[key(int(std::floor(p.x / kCell)), int(std::floor(p.y / kCell)))].push_back({p, spacing * 0.5f});

            const float scale = rng.range(minScale, maxScale);
            const glm::vec3 pos(p.x, h - 0.15f * scale, p.y);
            const glm::quat rot = glm::angleAxis(rng.range(0.0f, 6.28f), glm::vec3(0, 1, 0));
            const glm::mat4 m = glm::translate(glm::mat4(1.0f), pos) * glm::mat4_cast(rot) *
                                glm::scale(glm::mat4(1.0f), glm::vec3(scale));
            // custom.w > 0: standard.vert tints the vertex colours by custom.rgb (natural variation).
            const float t = rng.range(0.85f, 1.12f);
            groups[variant]->add(m, glm::vec4(t, t * rng.range(0.95f, 1.05f), t, 1.0f));
            if (!set.colliders.empty()) {
                const ve::Collider& c = set.colliders[set.colliders.size() == 1 ? 0 : variant];
                field.bodies.push_back(ve::Engine::get().physics().createStaticBody(c, pos, rot, glm::vec3(scale), kind));
            }
            ++done;
        }
        for (auto& g : groups) field.groups.push_back(g);
        return done;
    };

    auto savannah = [&](glm::vec2 p, float h) {
        return h > 0.6f && terrain.swampMask(p.x, p.y) < 0.3f && terrain.rockMask(p.x, p.y) < 0.4f;
    };
    const int baobabs = place(1, lib.baobabs, 70, 30.0f, 0.9f, 1.3f, 3000.0f, 3000.0f, savannah);
    // Acacias cluster in groves: accept more often where a low-frequency noise is high.
    const int acacias = place(2, lib.acacias, 2600, 9.0f, 0.8f, 1.3f, 3000.0f, 3000.0f, [&](glm::vec2 p, float h) {
        return savannah(p, h) && rng.next() < fbm(p * 0.008f, 2, 77) * 1.6f - 0.2f;
    });
    const int fever = place(3, lib.feverTrees, 900, 5.0f, 0.8f, 1.2f, 3000.0f, 3000.0f, [&](glm::vec2 p, float h) {
        return h > 0.2f && terrain.swampMask(p.x, p.y) > 0.35f;
    });
    const int boulders = place(4, lib.boulders, 160, 6.0f, 2.0f, 5.0f, 3000.0f, 3000.0f, [&](glm::vec2 p, float h) {
        const bool nearKopje = glm::length(p - Terrain::kKopje) < 55.0f || glm::length(p - glm::vec2(-50.0f, -140.0f)) < 30.0f;
        return h > 0.5f && nearKopje && terrain.rockMask(p.x, p.y) > 0.5f;
    });
    const int rocks = place(5, lib.rocks, 1500, 3.0f, 0.4f, 1.4f, 450.0f, 50.0f, [&](glm::vec2, float h) { return h > 0.3f; });
    const int bushes = place(6, lib.bushes, 7000, 2.5f, 0.7f, 1.4f, 350.0f, 60.0f, [&](glm::vec2, float h) { return h > 0.3f; });
    ve::log::info("Props (instanced): {} baobab, {} acacia, {} fever, {} boulders, {} rocks, {} bushes; {} static bodies",
                  baobabs, acacias, fever, boulders, rocks, bushes, field.bodies.size());
}

void buildGrass(ve::Scene& scene, ve::Renderer& r, const Terrain& terrain)
{
    // Custom shader: blades bend with the wind (frame.globalParams[0]).
    ve::ShaderDesc desc;
    desc.name = "Grass";
    desc.vertex = "grass.vert";
    desc.fragment = "grass.frag";
    desc.cull = ve::CullMode::None;
    desc.castShadows = false; // thousands of thin blades: skip for speed
    auto material = r.createMaterial(r.createShader(desc));

    const float radius = 210.0f;
    const float cell = 32.0f;
    Rng rng(7);
    std::unordered_map<int64_t, ve::MeshData> chunks;
    size_t clumps = 0;

    auto addBlade = [&](ve::MeshData& md, glm::vec3 base, float yaw, float height, float width, glm::vec3 col,
                        float phase) {
        const glm::vec3 side(std::cos(yaw) * width, 0.0f, std::sin(yaw) * width);
        const glm::vec3 bend = glm::vec3(std::sin(yaw + 1.3f), 0.0f, std::cos(yaw + 1.3f)) * height * 0.25f;
        const auto first = static_cast<uint32_t>(md.vertices.size());
        ve::Vertex a, b, c;
        a.position = base - side;
        b.position = base + side;
        c.position = base + glm::vec3(0, height, 0) + bend;
        a.normal = b.normal = c.normal = glm::vec3(0, 1, 0);
        a.uv = {phase, 0.0f};
        b.uv = {phase, 0.0f};
        c.uv = {phase, 1.0f}; // uv.y = sway weight
        a.color = b.color = glm::vec4(col * 0.7f, 1.0f);
        c.color = glm::vec4(col * 1.15f, 1.0f);
        md.vertices.insert(md.vertices.end(), {a, b, c});
        md.indices.insert(md.indices.end(), {first, first + 1, first + 2});
    };

    for (float x = -radius; x < radius; x += 2.2f) {
        for (float z = -radius; z < radius; z += 2.2f) {
            const glm::vec2 p = Terrain::kCamp + glm::vec2(x, z) + glm::vec2(rng.range(-1.0f, 1.0f), rng.range(-1.0f, 1.0f));
            if (glm::length(p - Terrain::kCamp) > radius || !terrain.inBounds(p.x, p.y, 60.0f)) continue;
            if (glm::length(p - Terrain::kCamp) < 9.0f) continue; // trampled camp floor
            const float h = terrain.height(p.x, p.y);
            const float swamp = terrain.swampMask(p.x, p.y);
            const bool reeds = swamp > 0.4f && h > -0.5f && h < 0.6f;
            if (!reeds && (h < 0.25f || terrain.rockMask(p.x, p.y) > 0.45f)) continue;
            // Patchy density driven by noise.
            const float density = reeds ? 0.9f : fbm(p * 0.03f, 2, 61) * 1.3f - 0.1f;
            if (rng.next() > density) continue;

            const int64_t key = (int64_t(std::floor(p.x / cell)) << 32) ^ int64_t(int32_t(std::floor(p.y / cell)));
            ve::MeshData& md = chunks[key];
            const glm::vec3 base(p.x, h - 0.05f, p.y);
            const float phase = rng.next();
            ++clumps;
            if (reeds) {
                for (int i = 0; i < 5; ++i) {
                    addBlade(md, base + glm::vec3(rng.range(-0.3f, 0.3f), 0, rng.range(-0.3f, 0.3f)), rng.range(0.0f, 6.28f),
                             rng.range(1.4f, 2.3f), 0.03f, glm::vec3(0.30f, 0.40f, 0.14f) * rng.range(0.85f, 1.15f), phase);
                }
            } else {
                const glm::vec3 dry = glm::mix(glm::vec3(0.78f, 0.63f, 0.32f), glm::vec3(0.55f, 0.55f, 0.26f),
                                               fbm(p * 0.012f, 3, 31));
                for (int i = 0; i < 4; ++i) {
                    addBlade(md, base + glm::vec3(rng.range(-0.25f, 0.25f), 0, rng.range(-0.25f, 0.25f)),
                             rng.range(0.0f, 6.28f), rng.range(0.45f, 1.05f), 0.05f, dry * rng.range(0.85f, 1.15f), phase);
                }
            }
        }
    }
    for (auto& [key, md] : chunks) {
        if (md.indices.empty()) continue;
        auto& go = scene.spawn("Grass");
        auto& mr = go.addComponent<ve::MeshRenderer>(ve::Mesh::create(r, md, {}, "grass"), material);
        mr.castShadows = false;
    }
    ve::log::info("Grass: {} clumps in {} chunks", clumps, chunks.size());
}

} // namespace savanna
