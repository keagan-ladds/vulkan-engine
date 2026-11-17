#include "ve/world/Grass.hpp"

#include "ve/core/Log.hpp"
#include "ve/render/Material.hpp"
#include "ve/render/Mesh.hpp"
#include "ve/render/Renderer.hpp"
#include "ve/render/Texture.hpp"
#include "ve/scene/Components.hpp"
#include "ve/scene/Scene.hpp"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>

namespace ve {

GrassPass::GrassPass(GrassDesc desc) : RenderPass(desc.name, RenderStage::Opaque, 10), m_desc(std::move(desc))
{
    if (!m_desc.height) throw std::runtime_error("GrassDesc::height is required");
}

GrassPass::~GrassPass()
{
    if (m_registered) renderer().removeShadowCaster(this);
}

MeshData GrassDesc::cardMeshCrossed(uint32_t quads, float width)
{
    MeshData md;
    quads = std::max(quads, 1u);
    for (uint32_t q = 0; q < quads; ++q) {
        const float a = float(q) / float(quads) * std::numbers::pi_v<float>;
        const glm::vec3 side = glm::vec3(std::cos(a), 0.0f, std::sin(a)) * (width * 0.5f);
        const glm::vec3 n(-side.z, 0.0f, side.x);
        const auto first = static_cast<uint32_t>(md.vertices.size());
        const glm::vec3 pos[4] = {-side, side, side + glm::vec3(0, 1, 0), -side + glm::vec3(0, 1, 0)};
        const glm::vec2 uv[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}}; // image top = card top
        for (int k = 0; k < 4; ++k) {
            Vertex v;
            v.position = pos[k];
            v.normal = glm::normalize(n);
            v.uv = uv[k];
            v.color = glm::vec4(1.0f);
            md.vertices.push_back(v);
        }
        md.indices.insert(md.indices.end(), {first, first + 1, first + 2, first, first + 2, first + 3});
    }
    return md;
}

GrassPass::Baked GrassPass::bake(const GrassDesc& d, glm::vec2 lo, glm::vec2 size, uint32_t res, uint32_t mres)
{
    Baked k;
    k.lo = lo;
    k.size = size;
    k.res = res = std::max(res, 2u);
    k.maskRes = mres = std::max(mres, 1u);

    // Heights: exact samples (manual bilinear in the shader; R32F isn't always filterable).
    k.heights.resize(size_t(res) * res);
    for (uint32_t z = 0; z < res; ++z) {
        for (uint32_t x = 0; x < res; ++x) {
            const glm::vec2 p = lo + size * glm::vec2(float(x), float(z)) / float(res - 1);
            k.heights[size_t(z) * res + x] = d.height(p);
        }
    }
    // Mask: x density, y colour mix, z height scale, w free (sampled at texel centres).
    k.mask.resize(size_t(mres) * mres * 4);
    for (uint32_t z = 0; z < mres; ++z) {
        for (uint32_t x = 0; x < mres; ++x) {
            const glm::vec2 p = lo + size * (glm::vec2(float(x), float(z)) + 0.5f) / float(mres);
            const glm::vec4 m = d.mask ? glm::clamp(d.mask(p), 0.0f, 1.0f) : glm::vec4(1.0f, 0.5f, 1.0f, 0.0f);
            for (int c = 0; c < 4; ++c) k.mask[(size_t(z) * mres + x) * 4 + c] = uint8_t(std::lround(m[c] * 255.0f));
        }
    }

    // Ground height range per tile, for frustum culling.
    k.tilesX = std::max(1, int(std::ceil(size.x / d.tileSize - 1e-3f)));
    k.tilesZ = std::max(1, int(std::ceil(size.y / d.tileSize - 1e-3f)));
    k.tileHeights.assign(size_t(k.tilesX) * k.tilesZ, glm::vec2(FLT_MAX, -FLT_MAX));
    // The ground is bilinear between samples, so a tile's extremes are at the
    // samples inside it or on its border: take both (coarse heightmaps may have
    // no sample inside a tile at all).
    auto heightAt = [&](glm::vec2 local) { // local = position - lo
        const glm::vec2 t = glm::clamp(local / size, 0.0f, 1.0f) * float(res - 1);
        const glm::ivec2 i = glm::min(glm::ivec2(glm::floor(t)), glm::ivec2(int(res) - 2));
        const glm::vec2 f = t - glm::vec2(i);
        auto h = [&](int x, int z) { return k.heights[size_t(z) * res + size_t(x)]; };
        return glm::mix(glm::mix(h(i.x, i.y), h(i.x + 1, i.y), f.x), glm::mix(h(i.x, i.y + 1), h(i.x + 1, i.y + 1), f.x), f.y);
    };
    const glm::vec2 cell = size / float(res - 1);
    for (int tz = 0; tz < k.tilesZ; ++tz) {
        for (int tx = 0; tx < k.tilesX; ++tx) {
            const glm::vec2 tlo = glm::vec2(float(tx), float(tz)) * d.tileSize;
            const glm::vec2 thi = glm::min(tlo + d.tileSize, size);
            glm::vec2 range(FLT_MAX, -FLT_MAX);
            auto add = [&](glm::vec2 p) {
                const float h = heightAt(p);
                range = {std::min(range.x, h), std::max(range.y, h)};
            };
            // Border: every sample-grid crossing along the four edges (+ corners).
            for (float x = std::ceil(tlo.x / cell.x) * cell.x; x <= thi.x; x += cell.x) { add({x, tlo.y}); add({x, thi.y}); }
            for (float z = std::ceil(tlo.y / cell.y) * cell.y; z <= thi.y; z += cell.y) { add({tlo.x, z}); add({thi.x, z}); }
            add(tlo); add(thi); add({tlo.x, thi.y}); add({thi.x, tlo.y});
            // Samples inside.
            const int x0 = int(std::ceil(tlo.x / cell.x)), x1 = int(std::floor(thi.x / cell.x));
            const int z0 = int(std::ceil(tlo.y / cell.y)), z1 = int(std::floor(thi.y / cell.y));
            for (int z = std::max(z0, 0); z <= std::min(z1, int(res) - 1); ++z) {
                for (int x = std::max(x0, 0); x <= std::min(x1, int(res) - 1); ++x) {
                    const float h = k.heights[size_t(z) * res + size_t(x)];
                    range = {std::min(range.x, h), std::max(range.y, h)};
                }
            }
            k.tileHeights[size_t(tz) * k.tilesX + tx] = range;
        }
    }
    return k;
}

void GrassPass::apply(Baked&& k)
{
    Renderer& r = renderer();
    m_height = Texture::create2D(r, k.res, k.res, VK_FORMAT_R32_SFLOAT, k.heights.data(), k.heights.size() * sizeof(float),
                                 false, SamplerType::NearestClamp, m_desc.name + " height");
    m_mask = Texture::create2D(r, k.maskRes, k.maskRes, VK_FORMAT_R8G8B8A8_UNORM, k.mask.data(), k.mask.size(), false,
                               SamplerType::LinearClamp, m_desc.name + " mask");
    m_area = std::move(k);
    m_area.heights = {}; // on the GPU now
    m_area.mask = {};
    for (Lod& lod : m_lods) {
        lod.material->setTexture(0, m_height);
        lod.material->setTexture(1, m_mask);
    }
}

glm::vec2 GrassPass::windowFor(glm::vec2 eye) const
{
    // Snapped to the tile grid (tiles seed their clumps from their world
    // position, so re-centring doesn't move a single blade) and kept inside
    // the bounds.
    const GrassDesc& d = m_desc;
    glm::vec2 lo = d.boundsMin + glm::floor((eye - glm::vec2(m_windowSize * 0.5f) - d.boundsMin) / d.tileSize) * d.tileSize;
    const glm::vec2 maxLo = d.boundsMin + glm::floor((d.boundsMax - d.boundsMin - m_windowSize) / d.tileSize) * d.tileSize;
    return glm::clamp(lo, d.boundsMin, glm::max(maxLo, d.boundsMin));
}

void GrassPass::setup(RenderGraph&)
{
    Renderer& r = renderer();
    const GrassDesc& d = m_desc;
    const glm::vec2 size = d.boundsMax - d.boundsMin;
    const uint32_t res = std::max(d.resolution, 2u);

    // Windowed when the window is clearly smaller than the bounds; otherwise one bake of everything.
    m_windowSize = 0.0f;
    if (d.window > 0.0f) {
        const float w = std::ceil(d.window / d.tileSize) * d.tileSize;
        if (w < std::min(size.x, size.y) * 0.75f) m_windowSize = w;
    }
    if (m_windowSize > 0.0f) {
        // Same sample spacing and mask texel size as the whole-bounds textures would have.
        const glm::vec2 spacing = size / float(res - 1);
        const glm::vec2 texel = size / float(std::max(d.maskResolution, 1u));
        glm::vec2 eye = (d.boundsMin + d.boundsMax) * 0.5f;
        if (const Camera* cam = r.frameData().scene ? r.frameData().scene->mainCamera() : nullptr) {
            const glm::vec3 p = cam->transform().position();
            eye = {p.x, p.z};
        }
        apply(bake(d, windowFor(eye), glm::vec2(m_windowSize), uint32_t(std::lround(m_windowSize / spacing.x)) + 1,
                   uint32_t(std::lround(m_windowSize / texel.x))));
    } else {
        apply(bake(d, d.boundsMin, size, res, d.maskResolution));
        m_desc.height = nullptr; // baked for good: release the callbacks and what they captured
        m_desc.mask = nullptr;
    }

    // LOD chain (or the single legacy LOD).
    std::vector<GrassLod> lods = d.lods;
    if (lods.empty()) lods.push_back({d.cardMesh, d.cardTexture, FLT_MAX, d.fragmentShader});
    m_lods.clear();
    float prev = 0.0f;
    for (size_t i = 0; i < lods.size(); ++i) {
        const GrassLod& src = lods[i];
        Lod lod;
        lod.cards = !src.mesh.vertices.empty();
        lod.minDistance = prev;
        lod.maxDistance = i + 1 == lods.size() ? FLT_MAX : std::max(src.maxDistance, prev);
        prev = lod.maxDistance;
        lod.mesh = Mesh::create(r, lod.cards ? src.mesh : bladeClump(), {}, d.name + " clump LOD" + std::to_string(i));
        ShaderDesc sd;
        sd.name = d.name;
        sd.vertex = d.vertexShader;
        sd.fragment = !src.fragmentShader.empty() ? src.fragmentShader : lod.cards ? "grass_card.frag" : "grass_field.frag";
        sd.cull = CullMode::None;
        sd.castShadows = d.castShadows;
        sd.shadowVertex = "grass_shadow.vert";
        sd.shadowFragment = lod.cards ? "grass_card_shadow.frag" : ""; // blades: depth only
        // Share shaders between LODs that use the same programs.
        for (const Lod& other : m_lods)
            if (other.cards == lod.cards && other.shader->desc().fragment == sd.fragment) lod.shader = other.shader;
        if (!lod.shader) lod.shader = r.createShader(sd);
        lod.material = r.createMaterial(lod.shader);
        lod.material->setTexture(0, m_height);
        lod.material->setTexture(1, m_mask);
        if (src.texture) lod.material->setTexture(2, src.texture);
        m_lods.push_back(std::move(lod));
    }
    updateMaterial();
    r.addShadowCaster(this);
    m_registered = true;
    log::info("{}: {}x{} tiles{}, {} height samples, {} clumps per full tile, {} LOD{}", d.name, m_area.tilesX, m_area.tilesZ,
              m_windowSize > 0.0f ? " (window around the camera)" : "", m_area.res,
              uint32_t(std::pow(d.tileSize / d.spacing, 2.0f)), m_lods.size(), m_lods.size() == 1 ? "" : "s");
}

MeshData GrassPass::bladeClump() const
{
    // One clump: crossed blades around the origin, unit height. uv.x = per-blade
    // random, uv.y = 0 root .. 1 tip; colour.r = blade height factor.
    const GrassDesc& d = m_desc;
    MeshData md;
    uint32_t seed = 12345u;
    auto rnd = [&] {
        seed = seed * 1664525u + 1013904223u;
        return float(seed >> 8) / 16777216.0f;
    };
    for (uint32_t i = 0; i < std::max(d.bladesPerClump, 1u); ++i) {
        const float a = (float(i) + rnd() * 0.6f) / float(d.bladesPerClump) * std::numbers::pi_v<float>;
        const float r = std::sqrt(rnd()) * d.clumpRadius;
        const float pa = rnd() * 2.0f * std::numbers::pi_v<float>;
        const glm::vec3 base(std::cos(pa) * r, 0.0f, std::sin(pa) * r);
        const glm::vec3 side(std::cos(a), 0.0f, std::sin(a));
        const glm::vec3 lean = glm::vec3(-side.z, 0.0f, side.x) * (rnd() - 0.5f) * 0.5f;
        const float hf = 0.7f + 0.3f * rnd();
        const float br = rnd();
        const auto first = static_cast<uint32_t>(md.vertices.size());
        Vertex v[3];
        v[0].position = base - side * 0.5f;
        v[1].position = base + side * 0.5f;
        v[2].position = base + glm::vec3(0.0f, 1.0f, 0.0f) + lean; // x/z of the side are scaled by bladeWidth in the shader
        for (int k = 0; k < 3; ++k) {
            v[k].normal = glm::vec3(0, 1, 0);
            v[k].uv = {br, k == 2 ? 1.0f : 0.0f};
            v[k].color = glm::vec4(hf, 0, 0, 1);
            v[k].tangent = glm::vec4(base, 0.0f); // blade root (for width scaling)
        }
        md.vertices.insert(md.vertices.end(), {v[0], v[1], v[2]});
        md.indices.insert(md.indices.end(), {first, first + 1, first + 2});
    }
    return md;
}

namespace {
// The benders nearest the camera, once per frame (shared by every grass pass).
void gatherBenders(FrameData& frame)
{
    static uint64_t lastFrame = ~0ull;
    if (frame.frameNumber == lastFrame || !frame.scene) return;
    lastFrame = frame.frameNumber;
    struct Near {
        float distance;
        glm::vec4 bender;
    };
    std::vector<Near> near;
    frame.scene->forEach<GrassBender>([&](GrassBender& b) {
        if (!b.isActive() || b.radius <= 0.0f || b.strength <= 0.0f) return;
        const glm::vec3 p = b.transform().position();
        // Strength scales the radius the shader sees (w); 0..1 strength maps to how far blades lean.
        near.push_back({glm::length(p - frame.camera.position), glm::vec4(p, b.radius * std::min(b.strength, 1.0f))});
    });
    std::sort(near.begin(), near.end(), [](const Near& a, const Near& b) { return a.distance < b.distance; });
    for (uint32_t i = 0; i < kMaxGrassBenders; ++i) frame.globals.grassBenders[i] = i < near.size() ? near[i].bender : glm::vec4(0.0f);
}
} // namespace

void GrassPass::prepare(FrameData& frame)
{
    gatherBenders(frame);
    if (m_windowSize > 0.0f) {
        // A finished re-bake replaces the window; otherwise start one once the
        // camera has crossed a quarter of it (the grass radius stays inside).
        if (m_pending.valid() && m_pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) apply(m_pending.get());
        const glm::vec2 eye(frame.camera.position.x, frame.camera.position.z);
        const glm::vec2 centre = m_area.lo + m_area.size * 0.5f;
        const glm::vec2 off = glm::abs(eye - centre);
        if (!m_pending.valid() && std::max(off.x, off.y) > m_windowSize * 0.25f) {
            const glm::vec2 lo = windowFor(eye);
            if (lo != m_area.lo) {
                m_pending = std::async(std::launch::async, &GrassPass::bake, m_desc, lo, m_area.size, m_area.res, m_area.maskRes);
            }
        }
    }
    updateMaterial();
}

void GrassPass::updateMaterial()
{
    const GrassDesc& d = m_desc;
    const glm::vec2 size = m_area.size;
    // Layout read by ve_grass.glsl.
    const float blend = m_lods.size() > 1 ? std::max(d.lodBlend, 0.0f) : 0.0f;
    for (Lod& lod : m_lods) {
        Material& m = *lod.material;
        m.setVector(0, {m_area.lo, 1.0f / size});
        m.setVector(1, {d.colorA, d.spacing});
        m.setVector(2, {d.colorB, d.radius});
        m.setVector(3, {d.minHeight, d.maxHeight, d.bladeWidth, d.fadeStart});
        m.setVector(4, {d.farDensity, float(d.windSlot), d.sway, d.maxTilt});
        m.setVector(5, {lod.cards ? 1.0f : 0.0f, d.alphaCutoff, 0.0f, 0.0f});
        m.setVector(6, {lod.minDistance, std::min(lod.maxDistance, 1e30f), blend, 0.0f});
    }
}

void GrassPass::declare(PassBuilder& b)
{
    Renderer& r = renderer();
    b.use(r.sceneColor(), Access::ColorAttachment);
    b.use(r.sceneNormal(), Access::ColorAttachment);
    b.use(r.sceneDepth(), Access::DepthAttachment);
}

glm::uvec2 GrassPass::drawTiles(RenderContext& ctx, const Lod& lod, const Frustum& frustum, float maxDistance,
                                 uint32_t viewIndex)
{
    Renderer& r = renderer();
    const GrassDesc& d = m_desc;
    const glm::vec3 camPos = ctx.frame->camera.position;
    const glm::vec2 eye(camPos.x, camPos.z);
    const glm::vec2 rel = eye - m_area.lo;
    const float radius = std::min(d.radius, maxDistance);
    const int reach = int(std::ceil(radius / d.tileSize)) + 1;
    const int cx = int(std::floor(rel.x / d.tileSize)), cz = int(std::floor(rel.y / d.tileSize));
    const float perTile = std::floor(d.tileSize / d.spacing) * std::floor(d.tileSize / d.spacing);

    // Tiles that may hold clumps of this LOD (its range widened by the dither).
    const float halfBlend = m_lods.size() > 1 ? std::max(d.lodBlend, 0.0f) * 0.5f : 0.0f;
    const float lodMin = lod.minDistance - halfBlend, lodMax = lod.maxDistance + halfBlend;
    if (lodMin > radius) return {0, 0};

    VkBuffer vb = lod.mesh->vertexBuffer();
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(ctx.cmd, 0, 1, &vb, &offset);
    vkCmdBindIndexBuffer(ctx.cmd, lod.mesh->indexBuffer(), 0, VK_INDEX_TYPE_UINT32);
    const auto indexCount = static_cast<uint32_t>(lod.mesh->submeshes()[0].indexCount);

    uint32_t tiles = 0, clumps = 0;
    for (int tz = std::max(0, cz - reach); tz <= std::min(m_area.tilesZ - 1, cz + reach); ++tz) {
        for (int tx = std::max(0, cx - reach); tx <= std::min(m_area.tilesX - 1, cx + reach); ++tx) {
            const glm::vec2 lo = m_area.lo + glm::vec2(float(tx), float(tz)) * d.tileSize;
            const glm::vec2 nearest = glm::clamp(eye, lo, lo + d.tileSize);
            const float dist = glm::length(nearest - eye);
            if (dist > radius || dist > lodMax) continue;
            const glm::vec2 farthest = glm::max(glm::abs(lo - eye), glm::abs(lo + d.tileSize - eye));
            if (glm::length(farthest) < lodMin) continue;
            const glm::vec2 hr = m_area.tileHeights[size_t(tz) * m_area.tilesX + tx];
            const AABB box{glm::vec3(lo.x - 1.0f, hr.x - 0.5f, lo.y - 1.0f),
                           glm::vec3(lo.x + d.tileSize + 1.0f, hr.y + d.maxHeight + 0.5f, lo.y + d.tileSize + 1.0f)};
            if (!frustum.intersects(box)) continue;
            // Clumps beyond the densest point of this tile are never visible: don't draw them.
            float keep = 1.0f;
            if (dist > d.fadeStart) {
                const float t = std::clamp((dist - d.fadeStart) / std::max(d.radius - d.fadeStart, 1.0f), 0.0f, 1.0f);
                keep = 1.0f + (d.farDensity - 1.0f) * t * t * (3.0f - 2.0f * t);
            }
            const auto count = static_cast<uint32_t>(std::ceil(perTile * keep));
            if (count == 0) continue;
            DrawPush push{};
            push.model = glm::mat4(1.0f);
            push.materialIndex = lod.material->gpuIndex();
            push.viewIndex = viewIndex;
            push.custom = glm::vec4(lo, d.tileSize, perTile);
            vkCmdPushConstants(ctx.cmd, r.pipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(DrawPush), &push);
            vkCmdDrawIndexed(ctx.cmd, indexCount, count, 0, 0, 0);
            ++tiles;
            clumps += count;
        }
    }
    return {tiles, clumps};
}

void GrassPass::execute(RenderContext& ctx)
{
    Renderer& r = renderer();
    ctx.beginRendering({{r.sceneColor()}, {r.sceneNormal()}}, {r.sceneDepth()});
    m_lastTiles = m_lastClumps = 0;
    for (Lod& lod : m_lods) {
        ctx.bindPipeline(lod.shader->pipeline(PassKind::Forward));
        const glm::uvec2 n = drawTiles(ctx, lod, ctx.frame->camera.frustum, FLT_MAX, 0);
        m_lastTiles += n.x;
        m_lastClumps += n.y;
        lod.clumps = n.y;
    }
    ctx.endRendering();
    m_lastShadowClumps = m_shadowClumps; // drawn earlier this frame by drawShadow()
    m_shadowClumps = 0;
}

void GrassPass::drawShadow(RenderContext& ctx, const ShadowView& view)
{
    if (!enabled || !m_desc.castShadows || view.texelSize > m_desc.shadowMaxTexel) return;
    for (const Lod& lod : m_lods) {
        ctx.bindPipeline(lod.shader->pipeline(PassKind::ShadowDirectional));
        m_shadowClumps += drawTiles(ctx, lod, view.frustum, m_desc.shadowDistance, view.viewIndex).y;
    }
}

void GrassPass::onGui()
{
    ImGui::Text("%u tiles, %u clumps drawn, %u in shadows", m_lastTiles, m_lastClumps, m_lastShadowClumps);
    if (m_lods.size() > 1) {
        for (size_t i = 0; i < m_lods.size(); ++i)
            ImGui::Text("LOD%zu: %u clumps (to %.0f m)", i, m_lods[i].clumps, std::min(m_lods[i].maxDistance, m_desc.radius));
        ImGui::SliderFloat("LOD blend", &m_desc.lodBlend, 0.0f, 20.0f);
    }
    ImGui::Checkbox("Cast shadows", &m_desc.castShadows);
    ImGui::SliderFloat("Shadow distance", &m_desc.shadowDistance, 0.0f, 100.0f);
    ImGui::SliderFloat("Shadow max texel", &m_desc.shadowMaxTexel, 0.01f, 0.5f);
    ImGui::SliderFloat("Radius", &m_desc.radius, 10.0f, 400.0f);
    ImGui::SliderFloat("Fade start", &m_desc.fadeStart, 0.0f, 200.0f);
    ImGui::SliderFloat("Far density", &m_desc.farDensity, 0.0f, 1.0f);
    ImGui::SliderFloat("Min height", &m_desc.minHeight, 0.05f, 3.0f);
    ImGui::SliderFloat("Max height", &m_desc.maxHeight, 0.05f, 3.0f);
    ImGui::SliderFloat("Blade width", &m_desc.bladeWidth, 0.005f, 0.2f);
    ImGui::SliderFloat("Sway", &m_desc.sway, 0.0f, 3.0f);
    ImGui::SliderFloat("Max tilt", &m_desc.maxTilt, 0.0f, 0.8f);
    if (std::any_of(m_lods.begin(), m_lods.end(), [](const Lod& l) { return l.cards; })) ImGui::SliderFloat("Alpha cutoff", &m_desc.alphaCutoff, 0.05f, 0.95f);
    ImGui::ColorEdit3("Colour A", &m_desc.colorA.x);
    ImGui::ColorEdit3("Colour B", &m_desc.colorB.x);
}

} // namespace ve
