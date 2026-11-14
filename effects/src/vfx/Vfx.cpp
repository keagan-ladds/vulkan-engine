#include "vfx/Vfx.hpp"

#include "vfx/Atmosphere.hpp"
#include "vfx/Bloom.hpp"
#include "vfx/Particles.hpp"
#include "vfx/VolumetricFog.hpp"

#include <ve/render/Renderer.hpp>

namespace vfx {

Effects addDefaultEffects(ve::Renderer& renderer)
{
    ve::RenderGraph& g = renderer.graph();
    Effects fx;
    fx.atmosphere = &g.addPass<AtmospherePass>();
    fx.fog = &g.addPass<VolumetricFogPass>();
    fx.particles = &g.addPass<ParticlePass>();
    fx.bloom = &g.addPass<BloomPass>();
    renderer.setGlobalParam(SlotWind, glm::vec4(0.8f, 0.6f, 3.0f, 1.0f));
    renderer.setGlobalParam(SlotWeather, glm::vec4(0.0f, 0.0f, 0.4f, 1.0f));
    return fx;
}

} // namespace vfx
