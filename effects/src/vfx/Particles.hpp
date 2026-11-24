#pragma once

// =============================================================================
// Particles - CPU-simulated billboard particles.
//
//   auto& fire = go.addComponent<vfx::ParticleEmitter>();
//   fire.settings.rate = 60;
//   fire.settings.colorStart = {4.0f, 1.6f, 0.4f, 1.0f};   // HDR: >1 glows with bloom
//   fire.settings.blend = ve::BlendMode::Additive;
//
// ParticleEmitter (a Component) spawns and simulates particles in update().
// ParticlePass (RenderStage::Transparent) draws every emitter as instanced,
// camera-facing (or velocity-stretched) soft quads.
// =============================================================================

#include "vfx/InstancedDraw.hpp"

#include <ve/gfx/Pipeline.hpp>
#include <ve/gfx/Resources.hpp>
#include <ve/render/RenderGraph.hpp>
#include <ve/scene/GameObject.hpp>

#include <glm/glm.hpp>

#include <array>
#include <memory>
#include <random>
#include <vector>

namespace ve {
class Texture;
}

namespace vfx {

/// How an emitter spawns, moves and draws its particles.
struct ParticleSettings {
    float rate = 20.0f;               ///< particles per second while emitting
    uint32_t maxParticles = 512;      ///< Alive at once; spawning pauses at the limit.
    float lifetimeMin = 1.0f;         ///< Shortest life (s).
    float lifetimeMax = 2.0f;         ///< Longest life (s).
    /// Spawn volume (local space of the emitter): a box, plus a random point in a sphere.
    glm::vec3 spawnBox{0.0f};
    float spawnRadius = 0.0f;         ///< Radius of the random sphere (m).
    /// Initial velocity = velocity + random in [-velocityRandom, +velocityRandom] (world space).
    glm::vec3 velocity{0.0f, 1.0f, 0.0f};
    glm::vec3 velocityRandom{0.2f};   ///< See velocity.
    glm::vec3 gravity{0.0f};          ///< acceleration (m/s^2)
    float drag = 0.0f;                ///< velocity damping per second
    float windInfluence = 0.0f;       ///< 0..1, pushes along globalParams[SlotWind]
    /// Collision: each particle casts one physics ray along its initial motion
    /// when it spawns and dies where that ray hits (roofs stop rain). Cheap and
    /// exact for straight-falling particles; gravity curves aren't followed.
    bool collide = false;
    float collisionDistance = 40.0f;  ///< ray length (m)
    float sizeStart = 0.2f;           ///< Size (m) when born...
    float sizeEnd = 0.5f;             ///< ...and when it dies.
    float sizeRandom = 0.3f;          ///< +-fraction
    /// Colour over life: start -> mid (at midPoint) -> end. RGB can exceed 1 (HDR).
    glm::vec4 colorStart{1.0f};
    glm::vec4 colorMid{1.0f};                   ///< See colorStart.
    glm::vec4 colorEnd{1.0f, 1.0f, 1.0f, 0.0f}; ///< See colorStart (alpha 0: fades out).
    float midPoint = 0.4f;                      ///< Fraction of the life (0..1) where colorMid applies.
    float rotationSpeed = 0.5f;       ///< random spin, radians/s
    ve::BlendMode blend = ve::BlendMode::AlphaBlend; ///< AlphaBlend or Additive
    bool stretchWithVelocity = false; ///< streaks (rain, sparks)
    float stretch = 0.04f;            ///< seconds of motion per streak
    float softness = 0.5f;            ///< soft-particle fade distance (m)
    std::shared_ptr<ve::Texture> texture; ///< null = built-in soft circle
};

/// Spawns and simulates CPU particles from its object (ParticlePass draws them).
class ParticleEmitter : public ve::Component {
public:
    /// Wind drift applied on top of particle velocities (drag-free particles), m/s.
    const glm::vec3& drift() const { return m_drift; }

    /// One live particle.
    struct Particle {
        glm::vec3 position; ///< World position.
        glm::vec3 velocity; ///< m/s.
        float age;          ///< Seconds since born.
        float lifetime;     ///< Seconds it lives.
        float sizeScale;    ///< Random size multiplier.
        float rotation;     ///< Billboard rotation (radians).
        float spin;         ///< Rotation speed (radians/s).
        float killY; ///< collide: dies below this height (-inf = no hit)
    };

    ParticleSettings settings;  ///< Change any time.
    bool emitting = true;       ///< Spawn at `rate` (false: live particles finish their lives).

    /// Spawns `count` particles at once (sparks, a puff).
    void burst(uint32_t count);
    /// Removes every live particle.
    void clear() { m_particles.clear(); }
    /// The live particles.
    const std::vector<Particle>& particles() const { return m_particles; }

    void update(float dt) override;

private:
    void spawn();
    float rand01() { return m_dist(m_rng); }

    std::vector<Particle> m_particles;
    glm::vec3 m_drift{0.0f};
    float m_accumulator = 0.0f;
    std::mt19937 m_rng{std::random_device{}()};
    std::uniform_real_distribution<float> m_dist{0.0f, 1.0f};
};

/// Draws every ParticleEmitter as instanced soft billboards (transparent stage).
class ParticlePass : public ve::RenderPass {
public:
    ParticlePass();
    ~ParticlePass() override;

    void setup(ve::RenderGraph& graph) override;
    void prepare(ve::FrameData& frame) override;
    void declare(ve::PassBuilder& builder) override;
    void execute(ve::RenderContext& ctx) override;
    void onGui() override;

    /// Built-in sprites, also handy for your own emitters.
    std::shared_ptr<ve::Texture> softSprite;
    std::shared_ptr<ve::Texture> smokeSprite; ///< A soft, noisy puff.

    static constexpr uint32_t kMaxInstances = 32768; ///< Particles drawn per frame at most (all emitters).

private:
    struct Instance {
        glm::vec4 posSize;
        glm::vec4 color;
        glm::vec4 velRot;
    };
    struct Draw {
        uint32_t first, count;
        bool additive;
        uint32_t texture, flags;
        float stretch, softness;
    };

    FrameBuffers m_instances; ///< per frame in flight
    std::vector<Draw> m_draws;
    std::vector<uint32_t> m_order;
    std::vector<float> m_distance; ///< squared camera distance per particle (sort keys)
    VkPipeline m_alpha = VK_NULL_HANDLE;
    VkPipeline m_additive = VK_NULL_HANDLE;
    uint32_t m_slot = 0;
    uint32_t m_lastCount = 0;
};

} // namespace vfx
