#pragma once

// =============================================================================
// Fire - procedural flames (no textures, no simulation).
//
//   auto& fire = campfire.addComponent<vfx::Fire>();
//   fire.height = 1.2f;              // flame height (m)
//   fire.radius = 0.45f;             // base radius
//   light.intensity = base * fire.flicker();   // the glow follows the flames
//
// Each Fire is a handful of crossed billboards that turn around the vertical
// axis to face the camera, each drawing flame tongues from scrolling,
// domain-warped noise shaped like a teardrop and coloured by a black-body
// ramp (deep red at the edges, orange, yellow, near-white at the hottest
// core). They lean with the wind (vfx::SlotWind). A horizontal layer of
// glowing coals sits underneath. Output is additive HDR: bloom makes the
// glow. Cost: a few small quads per fire, ~8 noise lookups per pixel.
//
// Heat haze: above each fire a sheet bends what's behind it (and the flame
// tips) with rising noise, sampling a copy of the screen around the fires.
// Smoke and sparks are separate (ParticleEmitter), as is the light.
// =============================================================================

#include "vfx/InstancedDraw.hpp"

#include <ve/gfx/Resources.hpp>
#include <ve/render/RenderGraph.hpp>
#include <ve/scene/GameObject.hpp>

#include <glm/glm.hpp>

#include <array>
#include <vector>

namespace vfx {

class Fire : public ve::Component {
public:
    float height = 1.2f;        // m, base to the tips
    float radius = 0.45f;       // m, base
    float intensity = 6.0f;     // HDR brightness of the hottest part
    float temperature = 1.0f;   // < 1 redder and lazier, > 1 whiter (and hotter-looking)
    float speed = 1.0f;         // animation rate
    int layers = 4;             // flame sheets (2-6; more = fuller, costs more)
    float coals = 1.0f;         // glowing ember bed brightness (0 = none: torches, candles)
    float windInfluence = 1.0f; // how far the wind bends the flames
    float haze = 1.0f;          // heat shimmer above the flames (0 = none)
    uint32_t seed = 1;          // different fires look different

    // 0.6..1.25: the flames' overall brightness right now (light intensity, sound).
    float flicker() const;
    // A small wobble for the light's position (m): shadows dance with the flames.
    glm::vec3 lightJitter() const;

    void update(float dt) override;

private:
    float m_time = 0.0f;
};

class FirePass : public ve::RenderPass {
public:
    FirePass();
    ~FirePass() override;

    void setup(ve::RenderGraph& graph) override;
    void prepare(ve::FrameData& frame) override;
    void declare(ve::PassBuilder& builder) override;
    void execute(ve::RenderContext& ctx) override;
    void onGui() override;

    static constexpr uint32_t kMaxInstances = 4096;

private:
    struct Instance {
        glm::vec4 baseHeight;  // xyz base, w height
        glm::vec4 shape;       // radius, intensity (haze: strength), seed, kind (0 flame sheet, 1 coals, 2 haze)
        glm::vec4 lean;        // xz lean of the tips (m), temperature, angle offset (radians)
        glm::vec4 anim;        // time (s, scaled by speed), sheet index, sheet count, unused
    };

    FrameBuffers m_instances; // per frame in flight
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipeline m_hazePipeline = VK_NULL_HANDLE;
    ve::TextureId m_hazeCopy{};    // the screen around the fires, sampled by the haze
    glm::vec4 m_hazeRect{0.0f};    // uv x0, y0, x1, y1 to copy
    uint32_t m_slot = 0;
    uint32_t m_count = 0;          // flames + coals
    uint32_t m_hazeCount = 0;      // haze sheets, after them
    uint32_t m_fires = 0;
};

} // namespace vfx
