#pragma once

// =============================================================================
// vfx - optional visual effects built on the public engine API.
//
//   auto fx = vfx::addDefaultEffects(engine.renderer());
//   fx.atmosphere->setSun(&sunLight);          // which DirectionalLight is the sun
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
class FirePass;

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
    FirePass* fire = nullptr;
    BloomPass* bloom = nullptr;
};

// Adds sky + clouds, volumetric fog, fire, particles and bloom with sensible defaults.
// Also registers the effects' scene types (registerTypes()).
Effects addDefaultEffects(ve::Renderer& renderer);

// Registers Fire, Weather, DayNightCycle and Water for scene files and the
// editor (called by addDefaultEffects; safe to call more than once).
// ParticleEmitter isn't a scene type yet: add it from code.
void registerTypes();

} // namespace vfx
