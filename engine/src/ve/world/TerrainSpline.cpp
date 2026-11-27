#include "ve/world/TerrainSpline.hpp"

#include "ve/assets/AssetManager.hpp"
#include "ve/core/Log.hpp"
#include "ve/render/Material.hpp"
#include "ve/render/Mesh.hpp"
#include "ve/render/Renderer.hpp"
#include "ve/scene/Reflection.hpp"
#include "ve/scene/Scene.hpp"
#include "ve/scene/SceneSerializer.hpp"
#include "ve/scene/Spline.hpp"

#include <algorithm>
#include <cmath>

namespace ve {

TerrainSpline::TerrainSpline() : GameObject("Road")
{
    runInEditor = true; // the surface follows live edits
}

TerrainSpline::~TerrainSpline() = default;

void TerrainSpline::start()
{
    if (!getComponent<Spline>()) {
        auto& s = addComponent<Spline>();
        s.persistent = persistent;
    }
}

void TerrainSpline::onDestroy()
{
    m_surface.reset();
}

std::string TerrainSpline::terrainState()
{
    Spline* s = getComponent<Spline>();
    std::string state = std::to_string(s ? s->version() : 0) + (activeInHierarchy() ? "+" : "-");
    if (const TypeInfo* t = TypeRegistry::get().find("TerrainSpline")) {
        Json fields = SceneSerializer::serializeFields(*t, this);
        // (Only the terrain's part: the surface mesh fields don't re-shape the land.)
        for (const char* k : {"material", "surfaceWidth", "uvLength", "surfaceLift", "castShadows"}) fields.erase(k);
        state += fields.dump();
    }
    return state;
}

glm::vec4 TerrainSpline::bounds()
{
    Spline* s = getComponent<Spline>();
    if (!s) return glm::vec4(1e30f, 1e30f, -1e30f, -1e30f);
    const auto path = s->path();
    if (path->empty()) return glm::vec4(1e30f, 1e30f, -1e30f, -1e30f);
    const float r = 0.5f * std::max(width, 0.0f) * path->maxWidth() + std::max(falloff, biomeFalloff) + 4.0f;
    return {path->boundsMin().x - r, path->boundsMin().z - r, path->boundsMax().x + r, path->boundsMax().z + r};
}

void TerrainSpline::update(float)
{
    Spline* spline = getComponent<Spline>();
    if (!spline) return;
    const Terrain* terrain = Terrain::find(scene());
    const uint64_t sv = spline->version(), tv = terrain ? terrain->version() : 0;
    // The surface lies on the reshaped ground: rebuilt after the terrain is.
    if (sv != m_splineVersion || tv != m_terrainVersion) m_dirty = true;
    if (!m_dirty) return;
    m_splineVersion = sv;
    m_terrainVersion = tv;
    m_dirty = false;
    buildSurface();
}

void TerrainSpline::buildSurface()
{
    m_surface.reset();
    Spline* spline = getComponent<Spline>();
    if (material.empty() || !spline) return;
    const auto path = spline->path();
    if (path->empty()) return;
    std::shared_ptr<Material> mat;
    try {
        mat = assets().loadMaterial(material);
    } catch (const std::exception& e) {
        log::warnOnce(name() + ": can't load material '" + material + "': " + e.what());
        return;
    }
    const Terrain* terrain = Terrain::find(scene());
    const float w = surfaceWidth > 0.0f ? surfaceWidth : width;
    constexpr int kColumns = 4;
    constexpr float kStep = 1.0f; // rows every metre: follows the terrain's 2 m samples
    MeshData mesh;
    const float len = path->length();
    const int rows = std::max(2, int(std::ceil(len / kStep)) + 1);
    for (int r = 0; r < rows; ++r) {
        const float d = std::min(float(r) * kStep, len);
        const SplinePath::Sample s = path->at(d);
        glm::vec3 dir(s.tangent.x, 0.0f, s.tangent.z);
        dir = glm::length(dir) > 1e-4f ? glm::normalize(dir) : glm::vec3(1, 0, 0);
        const glm::vec3 right(-dir.z, 0.0f, dir.x);
        const float half = 0.5f * w * s.width;
        for (int c = 0; c <= kColumns; ++c) {
            const float u = float(c) / float(kColumns);
            glm::vec3 p = s.position + right * ((u * 2.0f - 1.0f) * half);
            glm::vec3 n(0, 1, 0);
            if (terrain && terrain->inBounds(p.x, p.z)) {
                p.y = terrain->height(p.x, p.z);
                n = terrain->normal(p.x, p.z);
            } else {
                p.y += heightOffset;
            }
            Vertex v;
            v.position = p + n * surfaceLift;
            v.normal = n;
            v.uv = {u, d / std::max(uvLength, 0.01f)};
            v.tangent = glm::vec4(right, 1.0f);
            mesh.vertices.push_back(v);
        }
    }
    const uint32_t ring = kColumns + 1;
    for (uint32_t r = 0; r + 1 < uint32_t(rows); ++r)
        for (uint32_t c = 0; c < uint32_t(kColumns); ++c) {
            const uint32_t a = r * ring + c, b = a + 1, e = a + ring, f = e + 1;
            mesh.indices.insert(mesh.indices.end(), {a, e, b, b, e, f}); // counter-clockwise seen from above
        }
    InstanceGroupDesc desc;
    desc.name = name() + " surface";
    desc.lods.push_back({Mesh::create(renderer(), mesh, {}, name() + " surface"), {mat}});
    desc.castShadows = castShadows;
    m_surface = renderer().createInstanceGroup(desc, 1);
    m_surface->add(glm::mat4(1.0f));
}

void registerTerrainSplineTypes(TypeRegistry& r)
{
    r.add<TerrainSpline>("TerrainSpline", [](TypeBuilder<TerrainSpline>& t) {
        t.category("World");
        t.field("width", &TerrainSpline::width).range(0.0f, 1000.0f).speed(0.05f).tooltip("full width of the flat surface (m)");
        t.field("falloff", &TerrainSpline::falloff).range(0.0f, 1000.0f).speed(0.05f).tooltip("blends back into the land over this (m)");
        t.field("flatten", &TerrainSpline::flatten).tooltip("shape the ground to the road's height");
        t.field("followGround", &TerrainSpline::followGround).tooltip("height = the ground under it, smoothed (else the spline's height)");
        t.field("smoothing", &TerrainSpline::smoothing).range(0.0f, 1000.0f).speed(0.5f).tooltip("followGround: smoothing length (m)");
        t.field("heightOffset", &TerrainSpline::heightOffset).range(-100.0f, 100.0f).speed(0.01f).tooltip("< 0 sinks the road (m)");
        t.field("biome", &TerrainSpline::biome).tooltip("a Terrain biome painted under the road (empty = none)");
        t.field("biomeStrength", &TerrainSpline::biomeStrength).range(0.0f, 1.0f).speed(0.01f);
        t.field("biomeFalloff", &TerrainSpline::biomeFalloff).range(0.0f, 100.0f).speed(0.05f).tooltip("beyond the width (m)");
        t.field("material", &TerrainSpline::material).assets(".material").tooltip("surface mesh material (empty = no mesh)");
        t.field("surfaceWidth", &TerrainSpline::surfaceWidth).range(0.0f, 1000.0f).speed(0.05f).tooltip("0 = width");
        t.field("uvLength", &TerrainSpline::uvLength).range(0.01f, 1000.0f).speed(0.05f).tooltip("metres per texture repeat along");
        t.field("surfaceLift", &TerrainSpline::surfaceLift).range(0.0f, 1.0f).speed(0.005f);
        t.field("castShadows", &TerrainSpline::castShadows);
        t.onChanged([](TerrainSpline& s) { s.markDirty(); });
    });
}

} // namespace ve
