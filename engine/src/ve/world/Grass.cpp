#include "ve/world/Grass.hpp"

#include "ve/core/Log.hpp"
#include "ve/render/Material.hpp"
#include "ve/render/Mesh.hpp"
#include "ve/render/Renderer.hpp"
#include "ve/render/Texture.hpp"

#include <imgui.h>

#include <algorithm>
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

void GrassPass::setup(RenderGraph&)
{
    Renderer& r = renderer();
    const GrassDesc& d = m_desc;
    const glm::vec2 size = d.boundsMax - d.boundsMin;

    // Heights: exact samples (manual bilinear in the shader; R32F isn't always filterable).
    const uint32_t res = std::max(d.resolution, 2u);
    std::vector<float> heights(size_t(res) * res);
    for (uint32_t z = 0; z < res; ++z) {
        for (uint32_t x = 0; x < res; ++x) {
            const glm::vec2 p = d.boundsMin + size * glm::vec2(float(x), float(z)) / float(res - 1);
            heights[size_t(z) * res + x] = d.height(p);
        }
    }
    m_height = Texture::create2D(r, res, res, VK_FORMAT_R32_SFLOAT, heights.data(), heights.size() * sizeof(float), false,
                                 SamplerType::NearestClamp, d.name + " height");

    // Mask: x density, y colour mix, z height scale, w free (sampled at texel centres).
    const uint32_t mres = std::max(d.maskResolution, 1u);
    std::vector<uint8_t> mask(size_t(mres) * mres * 4);
    for (uint32_t z = 0; z < mres; ++z) {
        for (uint32_t x = 0; x < mres; ++x) {
            const glm::vec2 p = d.boundsMin + size * (glm::vec2(float(x), float(z)) + 0.5f) / float(mres);
            const glm::vec4 m = d.mask ? glm::clamp(d.mask(p), 0.0f, 1.0f) : glm::vec4(1.0f, 0.5f, 1.0f, 0.0f);
            for (int c = 0; c < 4; ++c) mask[(size_t(z) * mres + x) * 4 + c] = uint8_t(std::lround(m[c] * 255.0f));
        }
    }
    m_mask = Texture::create2D(r, mres, mres, VK_FORMAT_R8G8B8A8_UNORM, mask.data(), mask.size(), false,
                               SamplerType::LinearClamp, d.name + " mask");

    // Ground height range per tile, for frustum culling.
    m_tilesX = std::max(1, int(std::ceil(size.x / d.tileSize)));
    m_tilesZ = std::max(1, int(std::ceil(size.y / d.tileSize)));
    m_tileHeights.assign(size_t(m_tilesX) * m_tilesZ, glm::vec2(FLT_MAX, -FLT_MAX));
    for (uint32_t z = 0; z < res; ++z) {
        for (uint32_t x = 0; x < res; ++x) {
            const glm::vec2 p = size * glm::vec2(float(x), float(z)) / float(res - 1);
            const float h = heights[size_t(z) * res + x];
            // A sample on a tile border belongs to both tiles.
            for (int dz = -1; dz <= 0; ++dz) {
                for (int dx = -1; dx <= 0; ++dx) {
                    const int tx = int(std::floor(p.x / d.tileSize + 0.001f * float(dx)));
                    const int tz = int(std::floor(p.y / d.tileSize + 0.001f * float(dz)));
                    if (tx < 0 || tz < 0 || tx >= m_tilesX || tz >= m_tilesZ) continue;
                    glm::vec2& t = m_tileHeights[size_t(tz) * m_tilesX + tx];
                    t = {std::min(t.x, h), std::max(t.y, h)};
                }
            }
        }
    }

    const bool cards = !d.cardMesh.vertices.empty();
    MeshData md;
    if (cards) {
        md = d.cardMesh;
    } else {
        // One clump: crossed blades around the origin, unit height. uv.x = per-blade
        // random, uv.y = 0 root .. 1 tip; colour.r = blade height factor.
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
    }
    m_clump = Mesh::create(r, md, {}, d.name + " clump");

    ShaderDesc sd;
    sd.name = d.name;
    sd.vertex = d.vertexShader;
    sd.fragment = !d.fragmentShader.empty() ? d.fragmentShader : cards ? "grass_card.frag" : "grass_field.frag";
    sd.cull = CullMode::None;
    sd.castShadows = d.castShadows;
    sd.shadowVertex = "grass_shadow.vert";
    sd.shadowFragment = cards ? "grass_card_shadow.frag" : ""; // blades: depth only
    m_shader = r.createShader(sd);
    m_material = r.createMaterial(m_shader);
    m_material->setTexture(0, m_height);
    m_material->setTexture(1, m_mask);
    if (d.cardTexture) m_material->setTexture(2, d.cardTexture);
    updateMaterial();
    r.addShadowCaster(this);
    m_registered = true;
    log::info("{}: {}x{} tiles, {} height samples, {} clumps per full tile", d.name, m_tilesX, m_tilesZ, res,
              uint32_t(std::pow(d.tileSize / d.spacing, 2.0f)));
}

void GrassPass::updateMaterial()
{
    const GrassDesc& d = m_desc;
    const glm::vec2 size = d.boundsMax - d.boundsMin;
    // Layout read by ve_grass.glsl.
    m_material->setVector(0, {d.boundsMin, 1.0f / size});
    m_material->setVector(1, {d.colorA, d.spacing});
    m_material->setVector(2, {d.colorB, d.radius});
    m_material->setVector(3, {d.minHeight, d.maxHeight, d.bladeWidth, d.fadeStart});
    m_material->setVector(4, {d.farDensity, float(d.windSlot), d.sway, d.maxTilt});
    m_material->setVector(5, {d.cardMesh.vertices.empty() ? 0.0f : 1.0f, d.alphaCutoff, 0.0f, 0.0f});
}

void GrassPass::declare(PassBuilder& b)
{
    Renderer& r = renderer();
    b.use(r.sceneColor(), Access::ColorAttachment);
    b.use(r.sceneNormal(), Access::ColorAttachment);
    b.use(r.sceneDepth(), Access::DepthAttachment);
}

glm::uvec2 GrassPass::drawTiles(RenderContext& ctx, const Frustum& frustum, float maxDistance, uint32_t viewIndex)
{
    Renderer& r = renderer();
    const GrassDesc& d = m_desc;
    const glm::vec3 camPos = ctx.frame->camera.position;
    const glm::vec2 eye(camPos.x, camPos.z);
    const glm::vec2 rel = eye - d.boundsMin;
    const float radius = std::min(d.radius, maxDistance);
    const int reach = int(std::ceil(radius / d.tileSize)) + 1;
    const int cx = int(std::floor(rel.x / d.tileSize)), cz = int(std::floor(rel.y / d.tileSize));
    const float perTile = std::floor(d.tileSize / d.spacing) * std::floor(d.tileSize / d.spacing);

    VkBuffer vb = m_clump->vertexBuffer();
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(ctx.cmd, 0, 1, &vb, &offset);
    vkCmdBindIndexBuffer(ctx.cmd, m_clump->indexBuffer(), 0, VK_INDEX_TYPE_UINT32);
    const auto indexCount = static_cast<uint32_t>(m_clump->submeshes()[0].indexCount);

    uint32_t tiles = 0, clumps = 0;
    for (int tz = std::max(0, cz - reach); tz <= std::min(m_tilesZ - 1, cz + reach); ++tz) {
        for (int tx = std::max(0, cx - reach); tx <= std::min(m_tilesX - 1, cx + reach); ++tx) {
            const glm::vec2 lo = d.boundsMin + glm::vec2(float(tx), float(tz)) * d.tileSize;
            const glm::vec2 nearest = glm::clamp(eye, lo, lo + d.tileSize);
            const float dist = glm::length(nearest - eye);
            if (dist > radius) continue;
            const glm::vec2 hr = m_tileHeights[size_t(tz) * m_tilesX + tx];
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
            push.materialIndex = m_material->gpuIndex();
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
    updateMaterial();
    ctx.beginRendering({{r.sceneColor()}, {r.sceneNormal()}}, {r.sceneDepth()});
    ctx.bindPipeline(m_shader->pipeline(PassKind::Forward));
    const glm::uvec2 n = drawTiles(ctx, ctx.frame->camera.frustum, FLT_MAX, 0);
    ctx.endRendering();
    m_lastTiles = n.x;
    m_lastClumps = n.y;
    m_lastShadowClumps = m_shadowClumps; // drawn earlier this frame by drawShadow()
    m_shadowClumps = 0;
}

void GrassPass::drawShadow(RenderContext& ctx, const ShadowView& view)
{
    if (!enabled || !m_desc.castShadows || view.texelSize > m_desc.shadowMaxTexel) return;
    updateMaterial(); // shadows render before execute() in the frame
    ctx.bindPipeline(m_shader->pipeline(PassKind::ShadowDirectional));
    m_shadowClumps += drawTiles(ctx, view.frustum, m_desc.shadowDistance, view.viewIndex).y;
}

void GrassPass::onGui()
{
    ImGui::Text("%u tiles, %u clumps drawn, %u in shadows", m_lastTiles, m_lastClumps, m_lastShadowClumps);
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
    if (!m_desc.cardMesh.vertices.empty()) ImGui::SliderFloat("Alpha cutoff", &m_desc.alphaCutoff, 0.05f, 0.95f);
    ImGui::ColorEdit3("Colour A", &m_desc.colorA.x);
    ImGui::ColorEdit3("Colour B", &m_desc.colorB.x);
}

} // namespace ve
