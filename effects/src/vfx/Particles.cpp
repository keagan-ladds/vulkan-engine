#include "vfx/Particles.hpp"

#include "vfx/Noise.hpp"
#include "vfx/Vfx.hpp"

#include <ve/render/Renderer.hpp>
#include <ve/render/Texture.hpp>
#include <ve/scene/Scene.hpp>

#include <imgui.h>

#include <algorithm>
#include <cstring>

namespace vfx {

// -----------------------------------------------------------------------------
// ParticleEmitter
// -----------------------------------------------------------------------------

void ParticleEmitter::spawn()
{
    if (m_particles.size() >= settings.maxParticles) return;
    const ParticleSettings& s = settings;
    glm::vec3 local = (glm::vec3(rand01(), rand01(), rand01()) * 2.0f - 1.0f) * s.spawnBox;
    if (s.spawnRadius > 0.0f) {
        glm::vec3 d;
        do {
            d = glm::vec3(rand01(), rand01(), rand01()) * 2.0f - 1.0f;
        } while (glm::dot(d, d) > 1.0f);
        local += d * s.spawnRadius;
    }
    Particle p;
    p.position = transform().transformPoint(local);
    p.velocity = s.velocity + (glm::vec3(rand01(), rand01(), rand01()) * 2.0f - 1.0f) * s.velocityRandom;
    p.age = 0.0f;
    p.lifetime = glm::mix(s.lifetimeMin, s.lifetimeMax, rand01());
    p.sizeScale = 1.0f + (rand01() * 2.0f - 1.0f) * s.sizeRandom;
    p.rotation = rand01() * 6.2831853f;
    p.spin = (rand01() * 2.0f - 1.0f) * s.rotationSpeed;
    m_particles.push_back(p);
}

void ParticleEmitter::burst(uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i) spawn();
}

void ParticleEmitter::update(float dt)
{
    if (emitting && settings.rate > 0.0f) {
        m_accumulator += dt * settings.rate;
        while (m_accumulator >= 1.0f) {
            spawn();
            m_accumulator -= 1.0f;
        }
    }
    // Wind (from the weather's global param) pulls particles along: with drag
    // they accelerate towards the air velocity, without drag they just drift.
    const glm::vec4 windParam = renderer().globalParam(SlotWind);
    const glm::vec3 air = glm::vec3(windParam.x, 0.0f, windParam.y) * windParam.z * settings.windInfluence;
    const float pull = std::min(1.0f, settings.drag * dt);
    for (Particle& p : m_particles) {
        p.velocity += settings.gravity * dt;
        if (settings.drag > 0.0f) p.velocity += (air - p.velocity) * pull;
        p.position += (p.velocity + (settings.drag > 0.0f ? glm::vec3(0.0f) : air)) * dt;
        p.rotation += p.spin * dt;
        p.age += dt;
    }
    std::erase_if(m_particles, [](const Particle& p) { return p.age >= p.lifetime; });
}

// -----------------------------------------------------------------------------
// ParticlePass
// -----------------------------------------------------------------------------

namespace {
struct ParticlePush {
    uint32_t texture;
    uint32_t flags;
    float stretch;
    float softness;
};
} // namespace

ParticlePass::ParticlePass() : RenderPass("Particles", ve::RenderStage::Transparent, 10) {}

ParticlePass::~ParticlePass()
{
    ve::Renderer& r = renderer();
    r.destroyPipeline(m_alpha);
    r.destroyPipeline(m_additive);
    ve::Device* device = &r.device();
    for (ve::Buffer b : m_instances) {
        device->deferDestroy([device, b]() mutable { device->destroyBuffer(b); });
    }
}

void ParticlePass::setup(ve::RenderGraph&)
{
    ve::Renderer& r = renderer();
    for (auto& b : m_instances) {
        b = r.device().createBuffer(sizeof(Instance) * kMaxInstances, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                    ve::MemoryUsage::CpuToGpu, "particle instances");
    }

    const VkVertexInputBindingDescription binding{0, sizeof(Instance), VK_VERTEX_INPUT_RATE_INSTANCE};
    const VkVertexInputAttributeDescription attrs[3] = {
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, posSize)},
        {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, color)},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, velRot)},
    };
    auto build = [&](ve::BlendMode blend) {
        VkShaderModule vs = r.loadShader("particle.vert");
        VkShaderModule fs = r.loadShader("particle.frag");
        VkPipeline p = ve::PipelineBuilder()
                           .shaders(vs, fs)
                           .vertexInput({&binding, 1}, attrs)
                           .colorAttachment(ve::Renderer::kSceneColorFormat, blend)
                           .depthAttachment(ve::Renderer::kDepthFormat)
                           .depthTest(true, false, VK_COMPARE_OP_GREATER_OR_EQUAL)
                           .layout(r.pipelineLayout())
                           .build(r.device().handle());
        vkDestroyShaderModule(r.device().handle(), vs, nullptr);
        vkDestroyShaderModule(r.device().handle(), fs, nullptr);
        return p;
    };
    m_alpha = build(ve::BlendMode::AlphaBlend);
    m_additive = build(ve::BlendMode::Additive);

    const auto soft = generateSoftSprite(64);
    softSprite = ve::Texture::create2D(r, 64, 64, VK_FORMAT_R8G8B8A8_UNORM, soft.data(), soft.size(), true,
                                       ve::SamplerType::LinearClamp, "particle soft");
    const auto smoke = generateSmokeSprite(128);
    smokeSprite = ve::Texture::create2D(r, 128, 128, VK_FORMAT_R8G8B8A8_UNORM, smoke.data(), smoke.size(), true,
                                        ve::SamplerType::LinearClamp, "particle smoke");
}

void ParticlePass::prepare(ve::FrameData& frame)
{
    m_draws.clear();
    m_slot = static_cast<uint32_t>(renderer().frameNumber() % 2);
    if (!frame.scene) return;
    auto* out = static_cast<Instance*>(m_instances[m_slot].mapped);
    const glm::vec3 cam = frame.camera.position;

    struct Item {
        ParticleEmitter* emitter;
        float distance;
    };
    std::vector<Item> emitters;
    frame.scene->forEach<ParticleEmitter>([&](ParticleEmitter& e) {
        if (e.isActive() && !e.particles().empty()) {
            emitters.push_back({&e, glm::length(e.transform().position() - cam)});
        }
    });
    // Far emitters first so nearer translucent particles blend over them.
    std::sort(emitters.begin(), emitters.end(), [](const Item& a, const Item& b) { return a.distance > b.distance; });

    uint32_t count = 0;
    for (const Item& it : emitters) {
        const ParticleSettings& s = it.emitter->settings;
        const auto& ps = it.emitter->particles();
        m_order.resize(ps.size());
        for (uint32_t i = 0; i < ps.size(); ++i) m_order[i] = i;
        if (s.blend != ve::BlendMode::Additive) {
            std::sort(m_order.begin(), m_order.end(), [&](uint32_t a, uint32_t b) {
                return glm::dot(ps[a].position - cam, ps[a].position - cam) > glm::dot(ps[b].position - cam, ps[b].position - cam);
            });
        }
        const uint32_t first = count;
        for (uint32_t idx : m_order) {
            if (count >= kMaxInstances) break;
            const auto& p = ps[idx];
            const float t = std::clamp(p.age / p.lifetime, 0.0f, 1.0f);
            const glm::vec4 color = t < s.midPoint ? glm::mix(s.colorStart, s.colorMid, t / std::max(s.midPoint, 1e-4f))
                                                   : glm::mix(s.colorMid, s.colorEnd, (t - s.midPoint) / std::max(1.0f - s.midPoint, 1e-4f));
            const float size = glm::mix(s.sizeStart, s.sizeEnd, t) * p.sizeScale;
            out[count++] = {glm::vec4(p.position, size), color, glm::vec4(p.velocity, p.rotation)};
        }
        if (count == first) continue;
        const auto& tex = s.texture ? s.texture : softSprite;
        m_draws.push_back({first, count - first, s.blend == ve::BlendMode::Additive, tex->index(),
                           s.stretchWithVelocity ? 1u : 0u, s.stretch, s.softness});
    }
    m_lastCount = count;
}

void ParticlePass::declare(ve::PassBuilder& b)
{
    ve::Renderer& r = renderer();
    b.use(r.sceneColor(), ve::Access::ColorAttachment);
    b.use(r.sceneDepth(), ve::Access::DepthRead);
    b.use(r.sceneDepth(), ve::Access::Sampled);
}

void ParticlePass::execute(ve::RenderContext& ctx)
{
    if (m_draws.empty()) return;
    ve::Renderer& r = renderer();
    ve::DepthTarget depth;
    depth.texture = r.sceneDepth();
    depth.readOnly = true;
    ctx.beginRendering({{r.sceneColor()}}, depth);
    VkBuffer vb = m_instances[m_slot].handle;
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(ctx.cmd, 0, 1, &vb, &offset);
    for (const Draw& d : m_draws) {
        ctx.bindPipeline(d.additive ? m_additive : m_alpha);
        ctx.push(ParticlePush{d.texture, d.flags, d.stretch, d.softness});
        vkCmdDraw(ctx.cmd, 6, d.count, 0, d.first);
    }
    ctx.endRendering();
}

void ParticlePass::onGui()
{
    ImGui::Text("Particles drawn: %u (%zu emitters)", m_lastCount, m_draws.size());
}

} // namespace vfx
