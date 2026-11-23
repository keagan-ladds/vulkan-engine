#include "Stress.hpp"

#include "Building.hpp"
#include "SavannaLayout.hpp"

#include <ve/assets/AssetManager.hpp>
#include <ve/core/Log.hpp>
#include <ve/render/Mesh.hpp>
#include <ve/scene/Components.hpp>
#include <ve/render/Renderer.hpp>
#include <ve/scene/Scene.hpp>
#include <ve/world/Terrain.hpp>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <string>

namespace savanna {

namespace {

// Logs what the renderer drew (draw calls aren't in the [bench] lines).
class StressStats : public ve::GameObject {
public:
    StressStats() : GameObject("Stress stats") {}
    void update(float) override
    {
        ve::Renderer& r = renderer();
        if (r.frameNumber() % 30 != 0) return;
        const ve::RenderStats& s = r.stats();
        const ve::InstanceStats& is = r.instances().stats();
        ve::log::info("[stress] frame {}: draw calls {}, shadow draw calls {}, indirect draws {}, objects {} ({} batched static), "
                      "static batching {}",
                      r.frameNumber(), s.drawCalls, s.shadowDrawCalls, is.drawCalls, s.totalObjects, s.staticObjects,
                      r.settings().staticBatching ? "on" : "off");
    }
};

void spawnPieces(ve::Scene& scene, const ve::Terrain& terrain, int count)
{
    using Kind = BuildPiece::Kind;
    // Every kind but the fittings (a door's leaf is a dynamic body), in every material.
    constexpr Kind kKinds[] = {Kind::Foundation, Kind::Floor,  Kind::Wall,       Kind::WallDoor,
                               Kind::WallWindow, Kind::Railing, Kind::Post,       Kind::Beam,
                               Kind::RoofMetal,  Kind::RoofThatch, Kind::Stairs, Kind::Fence};
    constexpr int kKindCount = int(std::size(kKinds));
    const int columns = std::max(1, int(std::ceil(std::sqrt(float(count)))));
    const float spacing = 2.5f;
    // South of the camp (the huts are north of the fire), the first rows
    // within the campfire's light; leave room around the fire and the player start.
    const glm::vec2 origin = layout::kCamp + glm::vec2(-12.0f, 3.0f);
    const glm::vec2 fire = layout::kCamp, start(29.0f, 15.0f);
    int placed = 0;
    for (int i = 0; placed < count; ++i) {
        const glm::vec2 p = origin + glm::vec2(float(i % columns), float(i / columns)) * spacing;
        if (glm::length(p - fire) < 3.0f || glm::length(p - start) < 3.0f) continue;
        auto& piece = scene.spawn<BuildPiece>();
        piece.kind = kKinds[placed % kKindCount];
        piece.material = BuildPiece::Material((placed / kKindCount) % 4);
        piece.transform().setPosition({p.x, terrain.height(p.x, p.y), p.y});
        piece.transform().setRotation(glm::angleAxis(glm::radians(90.0f * float(placed % 4)), glm::vec3(0, 1, 0)));
        ++placed;
    }
    ve::log::info("[stress] {} building pieces", placed);
}

} // namespace

void spawnStress(ve::Scene& scene, const ve::Terrain& terrain, std::string_view spec)
{
    const size_t colon = spec.find(':');
    const std::string what(spec.substr(0, colon));
    const int count = colon == std::string_view::npos ? 1000 : std::max(0, std::atoi(std::string(spec.substr(colon + 1)).c_str()));
    if (what == "pieces") {
        spawnPieces(scene, terrain, count);
    } else {
        ve::log::warn("SAVANNA_STRESS: unknown '{}' (pieces:<count>)", what);
        return;
    }
    scene.spawn<StressStats>();
}

void spawnPaintTest(ve::Scene& scene, const ve::Terrain& terrain)
{
    // A grid (vertex paint needs vertices to paint) in local XY, facing +Z.
    const int nx = 48, ny = 30;
    const float w = 4.0f, h = 2.5f;
    auto blob = [](float u, float v, float cu, float cv, float r) {
        const float d = std::hypot((u - cu) * 1.6f, v - cv) / r;
        return std::clamp(1.0f - d * d, 0.0f, 1.0f);
    };
    ve::MeshData md;
    for (int j = 0; j <= ny; ++j) {
        for (int i = 0; i <= nx; ++i) {
            const float u = float(i) / nx, v = float(j) / ny;
            ve::Vertex vx;
            vx.position = {(u - 0.5f) * w, v * h, 0.0f};
            vx.normal = {0.0f, 0.0f, 1.0f};
            vx.uv = {u * w * 0.5f, (1.0f - v) * h * 0.5f};
            const float dirt = std::max(std::clamp((0.45f - v) / 0.45f, 0.0f, 1.0f), blob(u, v, 0.8f, 0.75f, 0.18f));
            const float wear = std::max(blob(u, v, 0.45f, 0.6f, 0.22f), blob(u, v, 0.15f, 0.85f, 0.12f));
            const float wet = std::clamp((0.4f - u) / 0.1f, 0.0f, 1.0f) * std::clamp((0.75f - v) / 0.3f, 0.0f, 1.0f);
            vx.color = {dirt, wear, wet, 1.0f};
            md.vertices.push_back(vx);
        }
    }
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            const uint32_t a = uint32_t(j * (nx + 1) + i), b = a + 1, c = a + uint32_t(nx + 1), d = c + 1;
            md.indices.insert(md.indices.end(), {a, b, d, a, d, c}); // counter-clockwise from +Z
        }
    }
    md.computeTangents();
    auto& go = scene.spawn("Paint test wall");
    const glm::vec3 p(31.6f, 0.0f, 8.0f);
    go.transform().setPosition({p.x, terrain.height(p.x, p.z) - 0.1f, p.z});
    go.transform().setLocalRotation(glm::angleAxis(glm::radians(30.0f), glm::vec3(0, 1, 0)));
    auto mesh = ve::Mesh::create(ve::GameObject::renderer(), md, {}, "paint test");
    go.addComponent<ve::MeshRenderer>(mesh, ve::GameObject::assets().loadMaterial("materials/painted_test.material"));
    ve::log::info("Paint test wall spawned (shader {})", go.getComponent<ve::MeshRenderer>()->material(0)->shader().desc().fragment);
}

} // namespace savanna

