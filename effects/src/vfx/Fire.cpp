#include "vfx/Fire.hpp"

#include "vfx/Vfx.hpp"

#include <ve/gfx/Pipeline.hpp>
#include <ve/render/Renderer.hpp>
#include <ve/scene/Scene.hpp>

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace vfx {

namespace {

// Smooth 1D value noise (CPU side of the flicker; the shader has its own).
float hash1(float n)
{
    return std::fmod(std::sin(n) * 43758.5453f, 1.0f) * 0.5f + 0.5f;
}
float noise1(float x)
{
    const float i = std::floor(x), f = x - i;
    const float u = f * f * (3.0f - 2.0f * f);
    return hash1(i) * (1.0f - u) + hash1(i + 1.0f) * u;
}

struct HazePush {
    uint32_t copy;       // bindless index of the screen copy
    uint32_t pad[3];
    glm::vec4 rect;      // the copied uv rectangle (outside it: no offset)
};

} // namespace

// -----------------------------------------------------------------------------
// Fire
// -----------------------------------------------------------------------------

void Fire::update(float dt)
{
    m_time += dt * speed;
}

float Fire::flicker() const
{
    // Two slow octaves and a fast one: breathing plus crackle.
    const float s = float(seed) * 17.31f;
    const float n = noise1(m_time * 2.1f + s) * 0.55f + noise1(m_time * 5.3f + s * 1.7f) * 0.3f + noise1(m_time * 13.0f + s * 2.3f) * 0.15f;
    return 0.6f + n * 0.65f;
}

glm::vec3 Fire::lightJitter() const
{
    const float s = float(seed) * 7.7f;
    return glm::vec3(noise1(m_time * 3.1f + s) - 0.5f, noise1(m_time * 2.3f + s + 31.0f) - 0.5f, noise1(m_time * 3.7f + s + 57.0f) - 0.5f) *
           glm::vec3(radius * 0.35f, height * 0.15f, radius * 0.35f);
}

// -----------------------------------------------------------------------------
// FirePass
// -----------------------------------------------------------------------------

FirePass::FirePass() : RenderPass("Fire", ve::RenderStage::Transparent, 9) {}

FirePass::~FirePass()
{
    ve::Renderer& r = renderer();
    r.destroyPipeline(m_pipeline);
    r.destroyPipeline(m_hazePipeline);
    m_instances.destroy(r.device());
}

void FirePass::setup(ve::RenderGraph&)
{
    ve::Renderer& r = renderer();
    m_instances.create(r.device(), sizeof(Instance) * kMaxInstances, "fire instances");
    const VkVertexInputBindingDescription binding{0, sizeof(Instance), VK_VERTEX_INPUT_RATE_INSTANCE};
    const VkVertexInputAttributeDescription attrs[4] = {
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, baseHeight)},
        {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, shape)},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, lean)},
        {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, anim)},
    };
    m_pipeline = buildInstancedPipeline(r, "fire.vert", "fire.frag", binding, attrs, ve::BlendMode::Additive);
    m_hazePipeline = buildInstancedPipeline(r, "fire.vert", "fire_haze.frag", binding, attrs, ve::BlendMode::AlphaBlend);
    m_hazeCopy = graph().createTexture("FireHazeCopy", {.format = ve::Renderer::kSceneColorFormat,
                                                        .extraUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT});
}

void FirePass::prepare(ve::FrameData& frame)
{
    m_count = 0;
    m_hazeCount = 0;
    m_fires = 0;
    m_slot = FrameBuffers::slot(renderer());
    if (!frame.scene) return;
    auto* out = static_cast<Instance*>(m_instances[m_slot].mapped);
    const glm::vec4 wind = renderer().globalParam(SlotWind);
    const float time = frame.time;
    std::vector<Instance> haze;
    glm::vec2 lo(1e30f), hi(-1e30f);
    bool fullScreen = false;
    frame.scene->forEach<Fire>([&](Fire& f) {
        if (!f.isActive() || f.height <= 0.0f) return;
        const int sheets = std::clamp(f.layers, 1, 8);
        if (m_count + uint32_t(sheets) + 1 > kMaxInstances) return;
        const glm::vec3 base = f.transform().position();
        const float scale = glm::length(glm::vec3(f.transform().worldMatrix()[1]));
        const float h = f.height * scale, rad = f.radius * scale;
        // Wind bends the tips downwind (more for tall flames, saturating in a gale).
        const float strength = wind.z * f.windInfluence;
        const glm::vec2 lean = glm::vec2(wind.x, wind.y) * h * 0.6f * (strength / (strength + 4.0f));
        const float seed = float(f.seed % 997u) * 1.618f;
        const float t = time * f.speed;
        for (int i = 0; i < sheets; ++i) {
            // Sheets fan out around the camera-facing direction, a little
            // apart, with their own noise: they read as one volume.
            const float angle = (float(i) - float(sheets - 1) * 0.5f) * (2.4f / float(sheets + 1));
            out[m_count++] = {glm::vec4(base, h), glm::vec4(rad * (1.0f - 0.08f * float(i & 1)), f.intensity, seed + float(i) * 3.71f, 0.0f),
                              glm::vec4(lean.x, lean.y, f.temperature, angle), glm::vec4(t, float(i), float(sheets), 0.0f)};
        }
        if (f.haze > 0.0f) {
            haze.push_back({glm::vec4(base, h), glm::vec4(rad, f.haze, seed, 2.0f), glm::vec4(lean.x, lean.y, 0.0f, 0.0f),
                            glm::vec4(t, 0.0f, 1.0f, 0.0f)});
            // The screen area it covers (its box, projected) is what gets copied.
            const float w = rad * 2.2f + glm::length(lean) * 1.5f;
            for (int c = 0; c < 8; ++c) {
                const glm::vec3 p = base + glm::vec3((c & 1) ? w : -w, (c & 2) ? h * 3.0f : h * 0.3f, (c & 4) ? w : -w);
                const glm::vec4 clip = frame.camera.viewProj * glm::vec4(p, 1.0f);
                if (clip.w <= 0.05f) {
                    fullScreen = true; // a corner behind the camera: copy everything
                    continue;
                }
                const glm::vec2 uv = glm::vec2(clip) / clip.w * 0.5f + 0.5f;
                lo = glm::min(lo, uv);
                hi = glm::max(hi, uv);
            }
        }
        if (f.coals > 0.0f)
            out[m_count++] = {glm::vec4(base, h), glm::vec4(rad * 1.25f, f.intensity * f.coals, seed, 1.0f),
                              glm::vec4(0.0f, 0.0f, f.temperature, 0.0f), glm::vec4(t, 0.0f, 1.0f, 0.0f)};
        ++m_fires;
    });
    for (const Instance& i : haze) {
        if (m_count + m_hazeCount >= kMaxInstances) break;
        out[m_count + m_hazeCount++] = i;
    }
    // Margin for the haze's offsets, clamped to the screen.
    m_hazeRect = fullScreen ? glm::vec4(0.0f, 0.0f, 1.0f, 1.0f)
                            : glm::clamp(glm::vec4(lo - 0.03f, hi + 0.03f), glm::vec4(0.0f), glm::vec4(1.0f));
    if (m_hazeRect.z <= m_hazeRect.x || m_hazeRect.w <= m_hazeRect.y) m_hazeCount = 0; // off screen
}

void FirePass::declare(ve::PassBuilder& b)
{
    ve::Renderer& r = renderer();
    b.use(r.sceneColor(), ve::Access::ColorAttachment);
    b.use(r.sceneDepth(), ve::Access::DepthRead);
    b.use(r.sceneDepth(), ve::Access::Sampled);
    b.use(m_hazeCopy, ve::Access::Sampled);
}

void FirePass::execute(ve::RenderContext& ctx)
{
    if (m_count == 0 && m_hazeCount == 0) return;
    ve::Renderer& r = renderer();
    ve::RenderGraph& g = graph();
    ve::DepthTarget depth;
    depth.texture = r.sceneDepth();
    depth.readOnly = true;
    VkBuffer vb = m_instances[m_slot].handle;
    VkDeviceSize offset = 0;
    if (m_count > 0) {
        ctx.beginRendering({{r.sceneColor()}}, depth);
        vkCmdBindVertexBuffers(ctx.cmd, 0, 1, &vb, &offset);
        ctx.bindPipeline(m_pipeline);
        vkCmdDraw(ctx.cmd, 6, m_count, 0, 0);
        ctx.endRendering();
    }
    if (m_hazeCount == 0) return;

    // Heat haze: copy the screen around the fires (flames included), then
    // draw the haze sheets sampling that copy with rising offsets.
    const VkExtent3D ext = g.get(r.sceneColor()).image.extent;
    const int32_t x0 = int32_t(m_hazeRect.x * float(ext.width)), y0 = int32_t(m_hazeRect.y * float(ext.height));
    const int32_t x1 = int32_t(std::ceil(m_hazeRect.z * float(ext.width))), y1 = int32_t(std::ceil(m_hazeRect.w * float(ext.height)));
    const ve::Image& copy = g.get(m_hazeCopy).image;
    const int32_t cx1 = std::min<int32_t>(x1, int32_t(copy.extent.width)), cy1 = std::min<int32_t>(y1, int32_t(copy.extent.height));
    if (cx1 <= x0 || cy1 <= y0) return;
    g.transition(ctx.cmd, r.sceneColor(), ve::Access::TransferSrc);
    g.transition(ctx.cmd, m_hazeCopy, ve::Access::TransferDst);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffset = region.dstOffset = {x0, y0, 0};
    region.extent = {uint32_t(cx1 - x0), uint32_t(cy1 - y0), 1};
    vkCmdCopyImage(ctx.cmd, g.get(r.sceneColor()).image.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy.handle,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    g.transition(ctx.cmd, m_hazeCopy, ve::Access::Sampled);
    g.transition(ctx.cmd, r.sceneColor(), ve::Access::ColorAttachment);

    ctx.beginRendering({{r.sceneColor()}}, depth);
    vkCmdBindVertexBuffers(ctx.cmd, 0, 1, &vb, &offset);
    ctx.bindPipeline(m_hazePipeline);
    ctx.push(HazePush{g.sampled(m_hazeCopy), {0, 0, 0}, m_hazeRect});
    vkCmdDraw(ctx.cmd, 6, m_hazeCount, 0, m_count);
    ctx.endRendering();
}

void FirePass::onGui()
{
    ImGui::Text("Fires: %u (%u quads, %u haze)", m_fires, m_count, m_hazeCount);
}

} // namespace vfx
