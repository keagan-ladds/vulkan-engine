#include "ve/world/SplineInstancer.hpp"

#include "ve/assets/AssetManager.hpp"
#include "ve/assets/ModelData.hpp"
#include "ve/core/Log.hpp"
#include "ve/physics/Physics.hpp"
#include "ve/render/Mesh.hpp"
#include "ve/render/Renderer.hpp"
#include "ve/scene/Reflection.hpp"
#include "ve/scene/Scene.hpp"
#include "ve/scene/Spline.hpp"
#include "ve/world/LodModel.hpp"
#include "ve/world/Terrain.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <sstream>

namespace ve {

namespace {

// Deterministic random in [0, 1) per (seed, item, channel).
float hash01(int seed, uint32_t item, uint32_t channel)
{
    uint32_t h = uint32_t(seed) * 0x9E3779B9u ^ item * 0x85EBCA6Bu ^ channel * 0xC2B2AE35u;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return float(h & 0xFFFFFFu) / float(0x1000000);
}

std::vector<glm::vec3> parseAttach(const std::string& spec)
{
    std::vector<glm::vec3> out;
    std::stringstream ss(spec);
    std::string part;
    while (std::getline(ss, part, ';')) {
        glm::vec3 v(0.0f);
        int n = 0;
        std::stringstream ps(part);
        std::string num;
        while (n < 3 && std::getline(ps, num, ',')) {
            try {
                v[n++] = std::stof(num);
            } catch (const std::exception&) {
                n = -1;
                break;
            }
        }
        if (n == 3) out.push_back(v);
    }
    return out;
}

// A tube along `points` (world space) appended to `mesh`.
void appendTube(MeshData& mesh, const std::vector<glm::vec3>& points, float radius, int sides, const glm::vec4& color)
{
    const uint32_t base = uint32_t(mesh.vertices.size());
    float along = 0.0f;
    for (size_t i = 0; i < points.size(); ++i) {
        const glm::vec3 a = points[i > 0 ? i - 1 : 0], b = points[std::min(i + 1, points.size() - 1)];
        const glm::vec3 t = glm::normalize(b - a + glm::vec3(0.0f, 0.0f, 1e-7f));
        glm::vec3 n1 = glm::cross(t, glm::vec3(0, 1, 0));
        n1 = glm::length(n1) > 1e-4f ? glm::normalize(n1) : glm::vec3(1, 0, 0);
        const glm::vec3 n2 = glm::cross(n1, t);
        if (i > 0) along += glm::length(points[i] - points[i - 1]);
        for (int s = 0; s <= sides; ++s) {
            const float ang = float(s) / float(sides) * 6.2831853f;
            const glm::vec3 n = n1 * std::cos(ang) + n2 * std::sin(ang);
            Vertex v;
            v.position = points[i] + n * radius;
            v.normal = n;
            v.uv = {along, float(s) / float(sides)};
            v.tangent = glm::vec4(t, 1.0f);
            v.color = color;
            mesh.vertices.push_back(v);
        }
    }
    const uint32_t ring = uint32_t(sides + 1);
    for (uint32_t i = 0; i + 1 < points.size(); ++i)
        for (uint32_t s = 0; s < uint32_t(sides); ++s) {
            const uint32_t a = base + i * ring + s, b = a + 1, c = a + ring, d = c + 1;
            mesh.indices.insert(mesh.indices.end(), {a, c, b, b, c, d});
        }
}

} // namespace

SplineInstancer::SplineInstancer() : GameObject("Spline instancer")
{
    runInEditor = true; // rebuilds live while points are dragged
}

SplineInstancer::~SplineInstancer() = default;

void SplineInstancer::start()
{
    if (!getComponent<Spline>()) {
        auto& s = addComponent<Spline>();
        s.persistent = persistent;
    }
}

void SplineInstancer::onDestroy()
{
    clearBodies();
    m_groups.clear();
    m_cables.reset();
}

void SplineInstancer::clearBodies()
{
    for (uint32_t b : m_bodies) physics().destroyStaticBody(b);
    m_bodies.clear();
}

void SplineInstancer::update(float)
{
    Spline* spline = getComponent<Spline>();
    if (!spline) return;
    const uint64_t sv = spline->version();
    const Terrain* terrain = followGround ? Terrain::find(scene()) : nullptr;
    const uint64_t tv = terrain ? terrain->version() : 0;
    if (sv != m_splineVersion || tv != m_terrainVersion) m_dirty = true;
    if (!m_dirty) return;
    m_splineVersion = sv;
    m_terrainVersion = tv;
    if (m_meshDirty) buildMeshes();
    rebuild();
    m_dirty = false;
}

void SplineInstancer::buildMeshes()
{
    m_meshDirty = false;
    m_variants.clear();
    m_halfExtents.clear();
    m_centres.clear();
    m_groups.clear();
    if (mesh.empty()) return;
    try {
        LodModel model = loadLodModel(assets(), mesh);
        Renderer& r = renderer();
        for (LodVariant& v : model.variants) {
            for (LodGeometry& g : v.levels)
                for (auto& m : g.materials)
                    if (!m) m = r.createStandardMaterial();
            AABB b = v.levels.front().data.bounds();
            m_halfExtents.push_back(b.valid() ? b.extents() : glm::vec3(0.1f));
            m_centres.push_back(b.valid() ? b.center() : glm::vec3(0.0f));
            LodChainDesc c;
            c.name = name() + "/" + v.name;
            const bool cookedLevels = v.cooked && !v.cooked->generated.empty();
            if (cookedLevels)
                for (const ModelLodLevelData& g : v.cooked->generated) v.levels.push_back({g.data, g.submeshes, v.levels.front().materials});
            c.cookedImpostor = v.cooked && v.cooked->impostor.valid() ? &v.cooked->impostor : nullptr;
            c.levels = std::move(v.levels);
            c.impostor = impostor;
            c.maxLevels = impostor || c.cookedImpostor ? 3 : 4;
            for (int i = 0; i < 4; ++i) {
                InstanceLod l;
                l.screenSize = std::array<float, 4>{0.25f, 0.08f, 0.025f, 0.0f}[size_t(i)];
                c.switches.push_back(l);
            }
            m_variants.push_back(buildLodChain(r, std::move(c)));
        }
    } catch (const std::exception& e) {
        log::warn("{}: can't load '{}': {}", name(), mesh, e.what());
        m_variants.clear();
    }
}

void SplineInstancer::rebuild()
{
    m_items.clear();
    clearBodies();
    m_cableCount = 0;
    Spline* spline = getComponent<Spline>();
    const auto path = spline ? spline->path() : nullptr;
    const Terrain* terrain = followGround ? Terrain::find(scene()) : nullptr;

    // Distances along the path.
    std::vector<float> ds;
    if (path && !path->empty()) {
        const float len = path->length();
        if (atPoints) {
            const size_t n = spline->points().size();
            for (size_t i = 0; i < n; ++i) ds.push_back(path->pointDistance(i));
        } else if (spacing > 0.01f) {
            const float from = std::clamp(startOffset, 0.0f, len), span = len - from;
            if (path->closed()) {
                const size_t n = std::max<size_t>(1, size_t(std::round(len / spacing)));
                for (size_t k = 0; k < n; ++k) ds.push_back(from + float(k) * len / float(n));
            } else if (fitEnds) {
                const size_t n = std::max<size_t>(1, size_t(std::round(span / spacing)));
                for (size_t k = 0; k <= n; ++k) ds.push_back(from + float(k) * span / float(n));
            } else {
                for (float d = from; d <= len + 1e-3f && ds.size() < 100000; d += spacing) ds.push_back(d);
            }
        }
    }

    // Item transforms (and which variant each one uses).
    std::vector<uint32_t> variantOf;
    const size_t variants = std::max<size_t>(1, m_variants.size());
    for (uint32_t i = 0; i < ds.size(); ++i) {
        const SplinePath::Sample s = path->at(ds[i]);
        glm::vec3 dir(s.tangent.x, 0.0f, s.tangent.z);
        dir = glm::length(dir) > 1e-4f ? glm::normalize(dir) : glm::vec3(1, 0, 0);
        const glm::vec3 right(-dir.z, 0.0f, dir.x);
        glm::vec3 pos = s.position + right * (offset.x + (hash01(seed, i, 1) * 2.0f - 1.0f) * jitter);
        glm::vec3 up(0, 1, 0);
        if (terrain && terrain->inBounds(pos.x, pos.z)) {
            pos.y = terrain->height(pos.x, pos.z);
            if (tiltWithGround) up = terrain->normal(pos.x, pos.z);
        }
        pos += up * offset.y;
        glm::quat rot = alignToPath ? glm::quat_cast(glm::mat3(dir, glm::vec3(0, 1, 0), right)) : glm::quat(1, 0, 0, 0);
        const float extraYaw = glm::radians(yaw + (hash01(seed, i, 2) * 2.0f - 1.0f) * randomYaw);
        rot = rot * glm::angleAxis(extraYaw, glm::vec3(0, 1, 0));
        if (tiltWithGround) rot = glm::rotation(glm::vec3(0, 1, 0), glm::normalize(up)) * rot;
        const float sc = glm::mix(scaleRange.x, scaleRange.y, hash01(seed, i, 3));
        m_items.push_back(glm::translate(glm::mat4(1.0f), pos) * glm::mat4_cast(rot) * glm::scale(glm::mat4(1.0f), glm::vec3(sc)));
        variantOf.push_back(uint32_t(hash01(seed, i, 4) * float(variants)) % uint32_t(variants));
    }

    // Instances: one group per variant, sized to fit (re-made when they grow).
    m_groups.resize(m_variants.size());
    for (size_t v = 0; v < m_variants.size(); ++v) {
        const uint32_t need = uint32_t(std::count(variantOf.begin(), variantOf.end(), uint32_t(v)));
        if (!m_groups[v] || m_groups[v]->capacity() < need) {
            InstanceGroupDesc d;
            d.name = name();
            d.lods = m_variants[v];
            d.castShadows = castShadows;
            d.cullDistance = viewDistance;
            d.shadowDistance = shadowDistance;
            m_groups[v] = renderer().createInstanceGroup(d, std::max(16u, need + need / 2));
        }
        m_groups[v]->clear();
    }
    for (size_t i = 0; i < m_items.size(); ++i) {
        if (m_variants.empty()) break;
        const uint32_t v = variantOf[i];
        m_groups[v]->add(m_items[i]);
        if (collision) {
            const glm::mat4& m = m_items[i];
            const glm::vec3 scale(glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2])));
            const glm::quat rot = glm::quat_cast(glm::mat3(glm::vec3(m[0]) / scale.x, glm::vec3(m[1]) / scale.y, glm::vec3(m[2]) / scale.z));
            const glm::vec3 centre = glm::vec3(m * glm::vec4(m_centres[v], 1.0f));
            m_bodies.push_back(physics().createStaticBody(Collider::box(glm::max(m_halfExtents[v] * scale, glm::vec3(0.02f))), centre, rot));
        }
    }

    // Cables between consecutive items.
    m_cables.reset();
    const std::vector<glm::vec3> attach = parseAttach(cableAttach);
    if (!attach.empty() && m_items.size() >= 2) {
        MeshData cables;
        const int sides = std::clamp(cableSides, 3, 12);
        const size_t spans = path->closed() && !atPoints ? m_items.size() : m_items.size() - 1;
        const bool wrapPoints = path->closed() && atPoints;
        const size_t total = wrapPoints ? m_items.size() : spans;
        for (size_t i = 0; i < total; ++i) {
            const glm::mat4& a = m_items[i];
            const glm::mat4& b = m_items[(i + 1) % m_items.size()];
            for (const glm::vec3& p : attach) {
                const glm::vec3 from(a * glm::vec4(p, 1.0f)), to(b * glm::vec4(p, 1.0f));
                const float span = glm::length(to - from);
                const int segs = std::clamp(int(span / 1.5f), 2, 64);
                std::vector<glm::vec3> pts;
                for (int k = 0; k <= segs; ++k) {
                    const float u = float(k) / float(segs);
                    // A parabola is close enough to a catenary for these sags.
                    pts.push_back(glm::mix(from, to, u) - glm::vec3(0.0f, 4.0f * cableSag * span * u * (1.0f - u), 0.0f));
                }
                appendTube(cables, pts, std::max(cableRadius, 0.001f), sides, glm::vec4(1.0f));
                ++m_cableCount;
            }
        }
        if (!cables.indices.empty()) {
            auto mat = renderer().createStandardMaterial();
            mat->setBaseColor(glm::vec4(cableColor, 1.0f));
            mat->setRoughness(0.55f);
            InstanceGroupDesc d;
            d.name = name() + " cables";
            d.lods.push_back({Mesh::create(renderer(), cables, {}, name() + " cables"), {mat}});
            d.castShadows = castShadows;
            d.cullDistance = viewDistance;
            d.shadowDistance = shadowDistance;
            m_cables = renderer().createInstanceGroup(d, 1);
            m_cables->add(glm::mat4(1.0f));
        }
    }
}

void registerSplineInstancerTypes(TypeRegistry& r)
{
    r.add<SplineInstancer>("SplineInstancer", [](TypeBuilder<SplineInstancer>& t) {
        t.category("World");
        t.field("mesh", &SplineInstancer::mesh).assets(".gltf .glb").tooltip("glTF model repeated along the spline (variants picked at random)");
        t.field("impostor", &SplineInstancer::impostor).tooltip("an octahedral impostor as the last LOD");
        t.field("viewDistance", &SplineInstancer::viewDistance).range(1.0f, 10000.0f).speed(5.0f);
        t.field("shadowDistance", &SplineInstancer::shadowDistance).range(0.0f, 10000.0f).speed(5.0f);
        t.field("castShadows", &SplineInstancer::castShadows);
        t.field("collision", &SplineInstancer::collision).tooltip("a static box collider per item");
        t.field("spacing", &SplineInstancer::spacing).range(0.05f, 1000.0f).speed(0.05f).tooltip("metres between items");
        t.field("atPoints", &SplineInstancer::atPoints).tooltip("one item at each control point instead of by spacing");
        t.field("fitEnds", &SplineInstancer::fitEnds).tooltip("adjust the spacing so items land on both ends");
        t.field("startOffset", &SplineInstancer::startOffset).range(0.0f, 10000.0f).speed(0.05f);
        t.field("offset", &SplineInstancer::offset).tooltip("x: to the right of the path, y: up (m)");
        t.field("followGround", &SplineInstancer::followGround).tooltip("drop items onto the terrain");
        t.field("tiltWithGround", &SplineInstancer::tiltWithGround);
        t.field("alignToPath", &SplineInstancer::alignToPath).tooltip("items' +X along the path");
        t.field("yaw", &SplineInstancer::yaw).range(-360.0f, 360.0f).tooltip("extra rotation about Y (degrees)");
        t.field("randomYaw", &SplineInstancer::randomYaw).range(0.0f, 180.0f);
        t.field("scaleRange", &SplineInstancer::scaleRange);
        t.field("jitter", &SplineInstancer::jitter).range(0.0f, 100.0f).speed(0.01f).tooltip("random sideways offset (m)");
        t.field("seed", &SplineInstancer::seed);
        t.field("cableAttach", &SplineInstancer::cableAttach).tooltip("cable points in the item's local space: \"x,y,z; x,y,z\"");
        t.field("cableSag", &SplineInstancer::cableSag).range(0.0f, 0.5f).speed(0.001f).tooltip("sag / span (0 = straight rails)");
        t.field("cableRadius", &SplineInstancer::cableRadius).range(0.001f, 1.0f).speed(0.001f);
        t.field("cableColor", &SplineInstancer::cableColor).color();
        t.field("cableSides", &SplineInstancer::cableSides).range(3.0f, 12.0f);
        t.onChanged([](SplineInstancer& s) { s.markDirty(); });
    });
}

} // namespace ve
