#include "vfx/Fire.hpp"
#include "vfx/Vfx.hpp"
#include <ve/scene/Reflection.hpp>
#include "vfx/Weather.hpp"
#include "vfx/Water.hpp"
#include "vfx/DayNight.hpp"

#include "vfx/Atmosphere.hpp"
#include "vfx/Bloom.hpp"
#include "vfx/Particles.hpp"
#include "vfx/VolumetricFog.hpp"

#include <ve/render/Renderer.hpp>

namespace vfx {

void registerTypes()
{
    static bool done = false;
    if (done) return;
    done = true;
    ve::TypeRegistry& r = ve::TypeRegistry::get();

    r.add<Fire>("Fire", [](ve::TypeBuilder<Fire>& t) {
        t.category("Effects");
        t.field("height", &Fire::height).range(0.01f, 20.0f).speed(0.01f);
        t.field("radius", &Fire::radius).range(0.01f, 10.0f).speed(0.01f);
        t.field("intensity", &Fire::intensity).range(0.0f, 100.0f).speed(0.05f);
        t.field("temperature", &Fire::temperature).range(0.3f, 2.0f).speed(0.01f);
        t.field("speed", &Fire::speed).range(0.0f, 5.0f).speed(0.01f);
        t.field("layers", &Fire::layers).range(1, 8);
        t.field("coals", &Fire::coals).range(0.0f, 4.0f).speed(0.01f);
        t.field("windInfluence", &Fire::windInfluence).range(0.0f, 4.0f).speed(0.01f);
    });
    r.add<Weather>("Weather", [](ve::TypeBuilder<Weather>& t) {
        t.category("Environment");
        t.enumField("startWeather", &Weather::startWeather, {"Clear", "Hazy", "Overcast", "Rain", "Thunderstorm"});
        t.field("windDirection", &Weather::windDirection);
        t.field("rainSound", &Weather::rainSound);
        t.field("stormSound", &Weather::stormSound);
        t.field("windSound", &Weather::windSound);
        t.field("rainVolume", &Weather::rainVolume).range(0.0f, 2.0f).speed(0.01f);
        t.field("stormVolume", &Weather::stormVolume).range(0.0f, 2.0f).speed(0.01f);
        t.field("windVolume", &Weather::windVolume).range(0.0f, 2.0f).speed(0.01f);
        t.field("stormBackground", &Weather::stormBackground).range(0.0f, 1.0f).speed(0.01f);
        t.field("lightning", &Weather::lightning);
        t.field("lightningInterval", &Weather::lightningInterval).range(1.0f, 120.0f);
        t.field("lightningStrength", &Weather::lightningStrength).range(0.0f, 10.0f);
        t.onChanged([](Weather& w) { w.setWeather(w.startWeather, 1.0f); });
    });
    r.add<DayNightCycle>("DayNightCycle", [](ve::TypeBuilder<DayNightCycle>& t) {
        t.category("Environment");
        t.field("timeOfDay", &DayNightCycle::timeOfDay).range(0.0f, 24.0f);
        t.field("dayLengthMinutes", &DayNightCycle::dayLengthMinutes).range(0.1f, 600.0f);
        t.field("sunIntensity", &DayNightCycle::sunIntensity).range(0.0f, 20.0f);
        t.field("moonIntensity", &DayNightCycle::moonIntensity).range(0.0f, 2.0f).speed(0.005f);
        t.field("nightExposure", &DayNightCycle::nightExposure).range(0.5f, 5.0f).speed(0.01f);
        t.field("advanceInEditor", &DayNightCycle::advanceInEditor);
        t.field("timeKeys", &DayNightCycle::timeKeys).tooltip("T / G fast-forward / rewind time");
    });
    r.add<Water>("Water", [](ve::TypeBuilder<Water>& t) {
        t.category("Environment");
        t.field("size", &Water::size).range(1.0f, 100000.0f);
        t.field("deepColor", &Water::deepColor).color();
        t.field("shallowColor", &Water::shallowColor).color();
        t.field("absorption", &Water::absorption).range(0.0f, 10.0f).speed(0.01f);
        t.field("shoreFoam", &Water::shoreFoam).range(0.0f, 2.0f).speed(0.01f);
        t.field("rippleScale", &Water::rippleScale).range(0.01f, 5.0f).speed(0.01f);
        t.field("rippleStrength", &Water::rippleStrength).range(0.0f, 1.0f).speed(0.005f);
        t.onChanged([](Water& w) { w.rebuild(); });
    });
}

Effects addDefaultEffects(ve::Renderer& renderer)
{
    registerTypes();
    ve::RenderGraph& g = renderer.graph();
    Effects fx;
    fx.atmosphere = &g.addPass<AtmospherePass>();
    fx.fog = &g.addPass<VolumetricFogPass>();
    fx.fire = &g.addPass<FirePass>();
    fx.particles = &g.addPass<ParticlePass>();
    fx.bloom = &g.addPass<BloomPass>();
    renderer.setGlobalParam(SlotWind, glm::vec4(0.8f, 0.6f, 3.0f, 1.0f));
    renderer.setGlobalParam(SlotWeather, glm::vec4(0.0f, 0.0f, 0.4f, 1.0f));
    return fx;
}

} // namespace vfx
