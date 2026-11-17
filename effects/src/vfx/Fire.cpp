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
    ve::Device* device = &r.device();
    for (ve::Buffer b : m_instances) device->deferDestroy([device, b]() mutable { device->destroyBuffer(b); });
}

void FirePass::setup(ve::RenderGraph&)
{
    ve::Renderer& r = renderer();
    for (auto& b : m_instances)
        b = r.device().createBuffer(sizeof(Instance) * kMaxInstances, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, ve::MemoryUsage::CpuToGpu,
                                    "fire instances");
    const VkVertexInputBindingDescription binding{0, sizeof(Instance), VK_VERTEX_INPUT_RATE_INSTANCE};
    const VkVertexInputAttributeDescription attrs[4] = {
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, baseHeight)},
        {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, shape)},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, lean)},
        {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, anim)},
    };
    VkShaderModule vs = r.loadShader("fire.vert");
    VkShaderModule fs = r.loadShader("fire.frag");
    m_pipeline = ve::PipelineBuilder()
                     .shaders(vs, fs)
                     .vertexInput({&binding, 1}, attrs)
                     .colorAttachment(ve::Renderer::kSceneColorFormat, ve::BlendMode::Additive)
                     .depthAttachment(ve::Renderer::kDepthFormat)
                     .depthTest(true, false, VK_COMPARE_OP_GREATER_OR_EQUAL)
                     .layout(r.pipelineLayout())
                     .build(r.device().handle());
    vkDestroyShaderModule(r.device().handle(), vs, nullptr);
    vkDestroyShaderModule(r.device().handle(), fs, nullptr);
}

void FirePass::prepare(ve::FrameData& frame)
{
    m_count = 0;
    m_fires = 0;
    m_slot = static_cast<uint32_t>(renderer().frameNumber() % 2);
    if (!frame.scene) return;
    auto* out = static_cast<Instance*>(m_instances[m_slot].mapped);
    const glm::vec4 wind = renderer().globalParam(SlotWind);
    const float time = frame.time;
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
        if (f.coals > 0.0f)
            out[m_count++] = {glm::vec4(base, h), glm::vec4(rad * 1.25f, f.intensity * f.coals, seed, 1.0f),
                              glm::vec4(0.0f, 0.0f, f.temperature, 0.0f), glm::vec4(t, 0.0f, 1.0f, 0.0f)};
        ++m_fires;
    });
}

void FirePass::declare(ve::PassBuilder& b)
{
    ve::Renderer& r = renderer();
    b.use(r.sceneColor(), ve::Access::ColorAttachment);
    b.use(r.sceneDepth(), ve::Access::DepthRead);
    b.use(r.sceneDepth(), ve::Access::Sampled);
}

void FirePass::execute(ve::RenderContext& ctx)
{
    if (m_count == 0) return;
    ve::Renderer& r = renderer();
    ve::DepthTarget depth;
    depth.texture = r.sceneDepth();
    depth.readOnly = true;
    ctx.beginRendering({{r.sceneColor()}}, depth);
    VkBuffer vb = m_instances[m_slot].handle;
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(ctx.cmd, 0, 1, &vb, &offset);
    ctx.bindPipeline(m_pipeline);
    vkCmdDraw(ctx.cmd, 6, m_count, 0, 0);
    ctx.endRendering();
}

void FirePass::onGui()
{
    ImGui::Text("Fires: %u (%u quads)", m_fires, m_count);
}

} // namespace vfx
