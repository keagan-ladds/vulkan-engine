#include "ve/render/passes/BuiltinPasses.hpp"

#include "ve/gfx/Pipeline.hpp"
#include "ve/render/Mesh.hpp"
#include "ve/render/Renderer.hpp"

#include <imgui.h>

#include <cstring>

namespace ve {

namespace {

// 64-bit hash of a point-shadow face's inputs (see RendererSettings::cachePointShadows).
struct FaceHash {
    uint64_t h = 0x9E3779B97F4A7C15ull;
    void add(uint64_t v)
    {
        h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        h *= 0xFF51AFD7ED558CCDull;
        h ^= h >> 33;
    }
    void addBytes(const void* data, size_t size)
    {
        const auto* p = static_cast<const unsigned char*>(data);
        for (; size >= 8; size -= 8, p += 8) {
            uint64_t v;
            std::memcpy(&v, p, 8);
            add(v);
        }
        uint64_t tail = 0;
        std::memcpy(&tail, p, size);
        add(tail);
    }
};

} // namespace

// -----------------------------------------------------------------------------
// InstanceCullPass
// -----------------------------------------------------------------------------

void InstanceCullPass::declare(PassBuilder& b)
{
    if (!renderer().settings().occlusionCulling) return;
    if (const auto* hiz = graph().findPass<HiZPass>(); hiz && hiz->lastFrame(renderer().frameNumber()))
        for (TextureId t : hiz->textures()) b.use(t, Access::Sampled);
}

void InstanceCullPass::execute(RenderContext& ctx)
{
    const auto* hiz = graph().findPass<HiZPass>();
    renderer().instances().setOcclusion(renderer().settings().occlusionCulling && hiz ? hiz->lastFrame(ctx.frame->frameNumber) : nullptr);
    renderer().instances().recordCulling(ctx.cmd, ctx.frameSlot);
}

void InstanceCullPass::onGui()
{
    const InstanceStats& s = renderer().instances().stats();
    ImGui::Text("Groups: %u   Batches: %u   Instances: %u", s.groups, s.batches, s.instances);
    ImGui::Text("Visible: %u camera, %u shadow (instance draws)", s.visibleCamera, s.visibleShadow);
    ImGui::Text("Indirect draws: %u", s.drawCalls);
    ImGui::Text("Camera per LOD: %u / %u / %u / %u", s.visibleLod[0], s.visibleLod[1], s.visibleLod[2], s.visibleLod[3]);
    RendererSettings& rs = renderer().settings();
    ImGui::SliderFloat("LOD bias", &rs.lodBias, 0.25f, 4.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    int shadowBias = static_cast<int>(rs.shadowLodBias);
    if (ImGui::SliderInt("Shadow LOD bias", &shadowBias, 0, 3)) rs.shadowLodBias = static_cast<uint32_t>(shadowBias);
    ImGui::Checkbox("LOD crossfade", &rs.lodCrossfade);
    ImGui::SliderFloat("Fade band", &rs.lodFadeBand, 0.01f, 0.5f);
}

// -----------------------------------------------------------------------------
// ShadowPass
// -----------------------------------------------------------------------------

void ShadowPass::declare(PassBuilder& b)
{
    bool anyDir = false, anyPoint = false;
    for (const auto& sv : renderer().frameData().shadowViews) {
        (sv.point ? anyPoint : anyDir) = true;
    }
    if (anyDir) b.use(renderer().dirShadowMap(), Access::DepthAttachment);
    if (anyPoint) b.use(renderer().pointShadowMap(), Access::DepthAttachment);
}

void ShadowPass::execute(RenderContext& ctx)
{
    Renderer& r = renderer();
    const FrameData& frame = *ctx.frame;
    const RendererSettings& s = r.settings();

    for (const ShadowView& sv : frame.shadowViews) {
        // Cull shadow casters against this view (cascade box or cube face +
        // light range). Casters outside the camera view still matter here!
        m_scratch.clear();
        for (uint32_t i = 0; i < frame.items.size(); ++i) {
            const DrawItem& it = frame.items[i];
            if (!it.castShadows) continue;
            if (sv.point) {
                const glm::vec3 closest = glm::clamp(sv.lightPosition, it.bounds.min, it.bounds.max);
                if (glm::length(closest - sv.lightPosition) > sv.range) continue;
            }
            if (!sv.frustum.intersects(it.bounds)) continue;
            m_scratch.push_back(i);
        }

        if (sv.point && s.cachePointShadows) {
            // Same light, same casters in the same places as last time: the
            // cube face still holds exactly what we'd render. Keep it.
            FaceHash fh;
            fh.add(r.contentEpoch());
            fh.addBytes(&sv.viewProj, sizeof(sv.viewProj));
            fh.add(uint64_t(sv.range * 1000.0f));
            for (uint32_t i : m_scratch) {
                const DrawItem& it = frame.items[i];
                fh.add(reinterpret_cast<uintptr_t>(it.mesh));
                fh.add(reinterpret_cast<uintptr_t>(it.submesh));
                fh.add(reinterpret_cast<uintptr_t>(&it.material->shader()));
                fh.addBytes(&it.world, sizeof(it.world));
                fh.addBytes(&r.materialData(it.material->gpuIndex()), sizeof(GpuMaterial)); // cutout textures/params
            }
            const uint64_t hash = fh.h | 1; // never 0 (= nothing cached)
            if (m_pointFaceHash.size() <= sv.layer) m_pointFaceHash.resize(sv.layer + 1, 0);
            if (m_pointFaceHash[sv.layer] == hash) {
                ++m_pointFacesCached;
                continue;
            }
            m_pointFaceHash[sv.layer] = hash;
            ++m_pointFacesRendered;
        } else if (sv.point && sv.layer < m_pointFaceHash.size()) {
            m_pointFaceHash[sv.layer] = 0;
        }

        DepthTarget depth;
        depth.texture = sv.point ? r.pointShadowMap() : r.dirShadowMap();
        depth.clear = true;
        depth.clearValue = 1.0f;
        depth.layer = static_cast<int>(sv.layer);
        ctx.beginRendering({}, depth);
        // Slope-scaled depth bias pushes depth away from the light to avoid
        // "shadow acne". Point shadows write their own depth (distance) in the
        // fragment shader, where raster bias doesn't apply; they bias in the shader.
        if (sv.point) vkCmdSetDepthBias(ctx.cmd, 0.0f, 0.0f, 0.0f);
        else vkCmdSetDepthBias(ctx.cmd, s.shadowDepthBiasConstant, 0.0f, s.shadowDepthBiasSlope);
        r.drawItems(ctx, m_scratch, sv.point ? PassKind::ShadowPoint : PassKind::ShadowDirectional, sv.viewIndex,
                    sv.lightIndex, sv.point ? kDrawFlagPointShadow : 0);
        if (!sv.point && sv.cullView != kInvalidIndex) {
            r.instances().draw(ctx, sv.cullView, PassKind::ShadowDirectional, sv.viewIndex);
        }
        if (!sv.point) {
            for (ShadowCaster* caster : r.shadowCasters()) caster->drawShadow(ctx, sv);
        }
        ctx.endRendering();
    }
}

void ShadowPass::onGui()
{
    RendererSettings& s = renderer().settings();
    ImGui::Checkbox("Visualize cascades", &s.debugCascades);
    ImGui::Checkbox("Cache static point shadows", &s.cachePointShadows);
    ImGui::Text("Point faces: %u rendered, %u reused (since start)", m_pointFacesRendered, m_pointFacesCached);
    ImGui::SliderFloat("Normal bias (texels)", &s.shadowNormalBias, 0.0f, 5.0f);
    ImGui::SliderFloat("Depth bias constant", &s.shadowDepthBiasConstant, 0.0f, 10.0f);
    ImGui::SliderFloat("Depth bias slope", &s.shadowDepthBiasSlope, 0.0f, 10.0f);
    ImGui::SliderFloat("PCF radius (texels)", &s.shadowPcfRadius, 0.0f, 3.0f);
    ImGui::SliderFloat("Split lambda", &s.cascadeSplitLambda, 0.0f, 1.0f);
    ImGui::SliderFloat("Max distance", &s.maxShadowDistance, 20.0f, 1000.0f);
    ImGui::SliderFloat("Sun direction step (deg)", &s.shadowDirectionStep, 0.0f, 1.0f, "%.2f");
    int cascades = static_cast<int>(s.cascadeCount);
    int dirCount = static_cast<int>(s.maxShadowedDirectionalLights);
    int pointCount = static_cast<int>(s.maxShadowedPointLights);
    bool changed = ImGui::SliderInt("Cascades", &cascades, 1, static_cast<int>(kMaxCascades));
    changed |= ImGui::SliderInt("Shadowed directional", &dirCount, 0, static_cast<int>(kMaxDirectionalLights));
    changed |= ImGui::SliderInt("Shadowed point", &pointCount, 0, 16);
    if (changed) {
        s.cascadeCount = static_cast<uint32_t>(cascades);
        s.maxShadowedDirectionalLights = static_cast<uint32_t>(dirCount);
        s.maxShadowedPointLights = static_cast<uint32_t>(pointCount);
        renderer().applySettings();
    }
}

// -----------------------------------------------------------------------------
// OpaquePass
// -----------------------------------------------------------------------------

void OpaquePass::declare(PassBuilder& b)
{
    Renderer& r = renderer();
    b.use(r.dirShadowMap(), Access::Sampled);
    b.use(r.pointShadowMap(), Access::Sampled);
    b.use(r.sceneColor(), Access::ColorAttachment);
    b.use(r.sceneNormal(), Access::ColorAttachment);
    b.use(r.sceneDepth(), Access::DepthAttachment);
}

void OpaquePass::execute(RenderContext& ctx)
{
    Renderer& r = renderer();
    // Reversed-Z: clear depth to 0 (= infinitely far).
    // Mesh pipelines render to SceneColor + SceneNormal (cleared to 0 = "no surface").
    ctx.beginRendering({{r.sceneColor(), true, clearColor}, {r.sceneNormal(), true, glm::vec4(0.0f)}},
                       {r.sceneDepth(), true, 0.0f});
    r.drawItems(ctx, ctx.frame->opaque, PassKind::Forward);
    r.instances().draw(ctx, /*camera view*/ 0, PassKind::Forward, 0);
    ctx.endRendering();
}

// -----------------------------------------------------------------------------
// TransparentPass
// -----------------------------------------------------------------------------

void TransparentPass::declare(PassBuilder& b)
{
    Renderer& r = renderer();
    b.use(r.dirShadowMap(), Access::Sampled);
    b.use(r.pointShadowMap(), Access::Sampled);
    b.use(r.sceneColor(), Access::ColorAttachment);
    b.use(r.sceneNormal(), Access::ColorAttachment); // bound (mesh pipelines have 2 targets) but not written
    // Depth is tested but not written, and can be sampled (frame.builtinTextures.x)
    // at the same time, e.g. for soft water edges.
    b.use(r.sceneDepth(), Access::DepthRead);
    b.use(r.sceneDepth(), Access::Sampled);
}

void TransparentPass::execute(RenderContext& ctx)
{
    if (ctx.frame->transparent.empty()) return;
    Renderer& r = renderer();
    DepthTarget depth{r.sceneDepth(), false, 0.0f, -1, true};
    ctx.beginRendering({{r.sceneColor()}, {r.sceneNormal()}}, depth);
    r.drawItems(ctx, ctx.frame->transparent, PassKind::Forward);
    ctx.endRendering();
}

// -----------------------------------------------------------------------------
// HiZPass
// -----------------------------------------------------------------------------

namespace {
struct HiZPush { // hiz.frag
    uint32_t source;   // SceneDepth (level 0) or the previous level
    uint32_t pad;
    glm::uvec2 sourceSize;
    glm::uvec2 size;   // this level's
};
constexpr uint32_t kHiZWidth = 512, kHiZHeight = 256;
} // namespace

HiZPass::~HiZPass()
{
    renderer().destroyPipeline(m_pipeline);
}

void HiZPass::setup(RenderGraph& g)
{
    for (uint32_t i = 0; i < kHiZLevels; ++i) {
        m_levels[i] = g.createTexture("HiZ" + std::to_string(i),
                                      {.format = VK_FORMAT_R32_SFLOAT,
                                       .width = std::max(kHiZWidth >> i, 1u),
                                       .height = std::max(kHiZHeight >> i, 1u),
                                       .sampler = SamplerType::NearestClamp});
    }
    m_pipeline = renderer().createFullscreenPipeline({.fragment = "hiz.frag", .colorFormats = {VK_FORMAT_R32_SFLOAT}});
}

void HiZPass::declare(PassBuilder& b)
{
    m_active = renderer().settings().occlusionCulling;
    if (!m_active) return;
    b.use(renderer().sceneDepth(), Access::Sampled);
    for (TextureId t : m_levels) b.use(t, Access::ColorAttachment);
}

void HiZPass::execute(RenderContext& ctx)
{
    if (!m_active) {
        m_builtFrame = ~0ull;
        return;
    }
    RenderGraph& g = graph();
    for (uint32_t i = 0; i < kHiZLevels; ++i) {
        const TextureId src = i == 0 ? renderer().sceneDepth() : m_levels[i - 1];
        const VkExtent3D e = g.get(src).image.extent, d = g.get(m_levels[i]).image.extent;
        ctx.fullscreen({{m_levels[i]}}, m_pipeline, HiZPush{g.sampled(src), 0u, {e.width, e.height}, {d.width, d.height}});
        g.transition(ctx.cmd, m_levels[i], Access::Sampled);
        m_indices[i] = g.sampled(m_levels[i]);
    }
    m_builtFrame = ctx.frame->frameNumber;
}

const std::array<uint32_t, kHiZLevels>* HiZPass::lastFrame(uint64_t frameNumber) const
{
    return m_builtFrame + 1 == frameNumber ? &m_indices : nullptr;
}

// -----------------------------------------------------------------------------
// VelocityPass
// -----------------------------------------------------------------------------

namespace {
struct VelocityPush { // velocity.vert
    glm::mat4 model;
    glm::mat4 prevModel;
};
} // namespace

VelocityPass::~VelocityPass()
{
    renderer().destroyPipeline(m_pipeline);
}

void VelocityPass::setup(RenderGraph& g)
{
    m_velocity = g.createTexture("Velocity", {.format = VK_FORMAT_R16G16_SFLOAT, .sampler = SamplerType::NearestClamp});
    Renderer& r = renderer();
    VkShaderModule vert = r.loadShader("velocity.vert"), frag = r.loadShader("velocity.frag");
    const auto binding = Vertex::binding();
    const auto attributes = Vertex::attributes();
    PipelineBuilder b;
    b.shaders(vert, frag)
        .vertexInput({&binding, 1}, attributes)
        .layout(r.pipelineLayout())
        .cull(CullMode::None)
        .colorAttachment(VK_FORMAT_R16G16_SFLOAT);
    m_pipeline = b.build(r.device().handle());
    vkDestroyShaderModule(r.device().handle(), vert, nullptr);
    vkDestroyShaderModule(r.device().handle(), frag, nullptr);
}

void VelocityPass::declare(PassBuilder& b)
{
    m_active = renderer().settings().temporalAA;
    if (!m_active) return;
    b.use(m_velocity, Access::ColorAttachment);
    b.use(renderer().sceneDepth(), Access::Sampled); // velocity.frag keeps only the visible surface
}

void VelocityPass::execute(RenderContext& ctx)
{
    if (!m_active) return;
    // Cleared to "not moving" (TAA then reprojects with the camera).
    ctx.beginRendering({{m_velocity, true, glm::vec4(100.0f)}});
    const FrameData& f = *ctx.frame;
    if (!f.moving.empty()) {
        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
        const Mesh* bound = nullptr;
        for (uint32_t idx : f.moving) {
            const DrawItem& item = f.items[idx];
            if (item.mesh != bound) {
                VkBuffer vb = item.mesh->vertexBuffer();
                VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(ctx.cmd, 0, 1, &vb, &offset);
                vkCmdBindIndexBuffer(ctx.cmd, item.mesh->indexBuffer(), 0, VK_INDEX_TYPE_UINT32);
                bound = item.mesh;
            }
            ctx.push(VelocityPush{item.world, item.prevWorld});
            vkCmdDrawIndexed(ctx.cmd, item.submesh->indexCount, 1, item.submesh->firstIndex, 0, 0);
        }
    }
    ctx.endRendering();
}

// -----------------------------------------------------------------------------
// TemporalAAPass
// -----------------------------------------------------------------------------

namespace {
struct TaaPush { // taa.frag
    uint32_t current;
    uint32_t history;
    uint32_t depth;
    uint32_t historyValid;
    float feedback;
    uint32_t velocity; // VelocityPass output (~0u = none)
};
} // namespace

TemporalAAPass::~TemporalAAPass()
{
    renderer().destroyPipeline(m_pipeline);
}

void TemporalAAPass::setup(RenderGraph& g)
{
    for (int i = 0; i < 2; ++i)
        m_history[i] = g.createTexture("TAAHistory" + std::to_string(i), {.format = Renderer::kSceneColorFormat});
    m_pipeline = renderer().createFullscreenPipeline({.fragment = "taa.frag", .colorFormats = {Renderer::kSceneColorFormat}});
}

void TemporalAAPass::declare(PassBuilder& b)
{
    m_active = renderer().settings().temporalAA;
    if (!m_active) return;
    Renderer& r = renderer();
    const uint32_t w = m_frame & 1u;
    b.use(r.sceneDepth(), Access::Sampled);
    b.use(r.sceneColor(), Access::Sampled); // then the copy target
    b.use(m_history[w], Access::ColorAttachment);
    b.use(m_history[w ^ 1u], Access::Sampled);
    if (auto* v = graph().findPass<VelocityPass>(); v && v->enabled) b.use(v->velocity(), Access::Sampled);
}

void TemporalAAPass::execute(RenderContext& ctx)
{
    if (!m_active) {
        m_frame = 0; // history is stale when turned back on
        return;
    }
    Renderer& r = renderer();
    RenderGraph& g = graph();
    const uint32_t w = m_frame & 1u;
    const Image& color = g.get(r.sceneColor()).image;
    const bool valid = m_frame > 0 && ctx.frame->frameNumber == m_lastFrame + 1 &&
                       color.extent.width == m_extent.width && color.extent.height == m_extent.height;
    m_extent = color.extent;
    m_lastFrame = ctx.frame->frameNumber;

    // 1. Resolve into this frame's history.
    const auto* vel = g.findPass<VelocityPass>();
    ctx.fullscreen({{m_history[w]}}, m_pipeline,
                   TaaPush{g.sampled(r.sceneColor()), g.sampled(m_history[w ^ 1u]), g.sampled(r.sceneDepth()), valid ? 1u : 0u,
                           feedback, vel && vel->enabled ? g.sampled(vel->velocity()) : ~0u});
    // 2. Copy it back over the scene colour for the following passes.
    g.transition(ctx.cmd, m_history[w], Access::TransferSrc);
    g.transition(ctx.cmd, r.sceneColor(), Access::TransferDst);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = color.extent;
    vkCmdCopyImage(ctx.cmd, g.get(m_history[w]).image.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, color.handle,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    ++m_frame;
}

void TemporalAAPass::onGui()
{
    ImGui::Checkbox("Enabled (jitters the camera)", &renderer().settings().temporalAA);
    ImGui::SliderFloat("Feedback", &feedback, 0.5f, 0.98f);
}

// -----------------------------------------------------------------------------
// CompositePass
// -----------------------------------------------------------------------------

namespace {
struct CompositePush {
    uint32_t sceneColor;
    float exposure;
    float contrast;
    float saturation;
    glm::vec4 tint;    // rgb, w = vignette
    int tonemapper;
    uint32_t graded;   // 1 = input is already graded + tonemapped
};
} // namespace

CompositePass::CompositePass()
    : FullscreenPass("Composite", RenderStage::Composite, 0,
                     {.fragment = "composite.frag", .output = "Backbuffer", .inputs = {"SceneColor"}})
{
}

void CompositePass::prepare(FrameData&)
{
    if (m_graded != kNoTexture) {
        setPush(CompositePush{graph().sampled(m_graded), 1.0f, 1.0f, 1.0f, glm::vec4(1.0f, 1.0f, 1.0f, vignette), 2, 1u});
    } else {
        setPush(CompositePush{inputIndex(0), effectiveExposure(), contrast, saturation, glm::vec4(tint, vignette), tonemapper, 0u});
    }
}

void CompositePass::declare(PassBuilder& b)
{
    FullscreenPass::declare(b);
    if (m_graded != kNoTexture) b.use(m_graded, Access::Sampled);
}

void CompositePass::execute(RenderContext& ctx)
{
    FullscreenPass::execute(ctx);
    m_graded = kNoTexture; // the grading pass sets it again next frame (if it's still enabled)
}

void CompositePass::onGui()
{
    if (m_graded != kNoTexture) ImGui::TextDisabled("Graded input: tonemapping is done by the grading pass");
    ImGui::SliderFloat("Exposure", &exposure, 0.05f, 8.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Contrast", &contrast, 0.5f, 1.5f);
    ImGui::SliderFloat("Saturation", &saturation, 0.0f, 2.0f);
    ImGui::SliderFloat("Vignette", &vignette, 0.0f, 1.0f);
    ImGui::ColorEdit3("Tint", &tint.x);
    ImGui::Combo("Tonemapper", &tonemapper, "ACES\0Reinhard\0None\0");
}

} // namespace ve
