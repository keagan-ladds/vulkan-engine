#pragma once

// =============================================================================
// vfx - optional visual effects built on the public engine API.
//
//   auto fx = vfx::addDefaultEffects(engine.renderer());
//   fx.atmosphere->sun = &sunLight;            // which DirectionalLight is the sun
//   fx.fog->density = 0.03f;
//
// Nothing in the engine depends on this module; every effect is just a
// RenderPass (or Component) you can add, remove, replace or copy.
//
// Opt-in passes (not added by addDefaultEffects; add with graph().addPass<T>()):
// ScreenSpaceGIPass, ScreenSpaceReflectionsPass, DepthOfFieldPass.
// =============================================================================

#include <cstdint>

namespace ve {
class Renderer;
}

namespace vfx {

class AtmospherePass;
class VolumetricFogPass;
class BloomPass;
class ParticlePass;

// Slots in ve::FrameGlobals::globalParams used by the effects (see vfx.glsl).
enum GlobalSlot : uint32_t {
    SlotWind = 0,     // xy dir, z strength, w gustiness
    SlotWeather = 1,  // x wetness, y rain, z cloud coverage, w fog multiplier
    SlotSun = 2,      // xyz towards sun, w illuminance
    SlotSunColor = 3, // rgb sun radiance after the atmosphere
};

struct Effects {
    AtmospherePass* atmosphere = nullptr;
    VolumetricFogPass* fog = nullptr;
    ParticlePass* particles = nullptr;
    BloomPass* bloom = nullptr;
};

// Adds sky + clouds, volumetric fog, particles and bloom with sensible defaults.
Effects addDefaultEffects(ve::Renderer& renderer);

} // namespace vfx
