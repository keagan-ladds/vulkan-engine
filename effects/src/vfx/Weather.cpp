#include "vfx/Weather.hpp"

#include "vfx/Atmosphere.hpp"
#include "vfx/Particles.hpp"
#include "vfx/Vfx.hpp"

#include <ve/audio/Audio.hpp>
#include <ve/render/Renderer.hpp>
#include <ve/render/passes/BuiltinPasses.hpp>
#include <ve/scene/Components.hpp>
#include <ve/scene/Scene.hpp>

#include <algorithm>
#include <cmath>

namespace vfx {

namespace {
Weather::State lerp(const Weather::State& a, const Weather::State& b, float t)
{
    auto m = [t](float x, float y) { return x + (y - x) * t; };
    return {m(a.coverage, b.coverage), m(a.cloudDensity, b.cloudDensity), m(a.fog, b.fog), m(a.haze, b.haze),
            m(a.wind, b.wind),         m(a.rain, b.rain),                 m(a.sunScale, b.sunScale),
            m(a.thunder, b.thunder)};
}
} // namespace

Weather::Weather() : GameObject("Weather")
{
    runInEditor = true; // sky, fog and sounds follow edits while the game is paused
    //                                  coverage density fog  haze  wind  rain  sun   thunder
    presets[size_t(Type::Clear)] =        {0.30f, 0.8f, 0.7f, 1.0f, 2.5f,  0.0f, 1.00f, 0.0f};
    presets[size_t(Type::Hazy)] =         {0.45f, 1.0f, 1.3f, 2.2f, 4.0f,  0.0f, 0.85f, 0.0f};
    presets[size_t(Type::Overcast)] =     {0.85f, 1.3f, 1.8f, 2.0f, 7.0f,  0.0f, 0.30f, 0.0f};
    presets[size_t(Type::Rain)] =         {0.97f, 1.8f, 1.8f, 2.0f, 9.0f,  1.0f, 0.15f, 0.0f};
    presets[size_t(Type::Thunderstorm)] = {1.00f, 2.3f, 2.2f, 2.0f, 14.0f, 1.0f, 0.07f, 1.0f};
    m_current = m_from = presets[size_t(Type::Clear)];
}

const char* Weather::name(Type type)
{
    switch (type) {
    case Type::Clear: return "Clear";
    case Type::Hazy: return "Hazy";
    case Type::Overcast: return "Overcast";
    case Type::Rain: return "Rain";
    case Type::Thunderstorm: return "Thunderstorm";
    default: return "?";
    }
}

void Weather::setWeather(Type type, float transitionSeconds)
{
    m_explicit = true;
    m_from = m_current;
    m_target = type;
    m_t = 0.0f;
    m_duration = std::max(transitionSeconds, 0.001f);
}

void Weather::next(float transitionSeconds)
{
    setWeather(static_cast<Type>((static_cast<int>(m_target) + 1) % static_cast<int>(Type::Count)), transitionSeconds);
}

void Weather::start()
{
    if (!m_explicit) {
        setWeather(startWeather, 0.01f);
        m_explicit = false;
    }
    // Rain is a particle emitter that follows the camera around.
    auto& rainObj = scene().spawn("Rain");
    rainObj.hideInHierarchy = true; // owned by this Weather: edit Weather, not its rain
    m_rainId = rainObj.id();
    m_rain = &rainObj.addComponent<ParticleEmitter>();
    ParticleSettings& s = m_rain->settings;
    s.rate = 0.0f;
    s.maxParticles = 4000;
    s.lifetimeMin = 0.8f;
    s.lifetimeMax = 1.1f;
    s.spawnBox = {22.0f, 0.5f, 22.0f};
    s.velocity = {0.0f, -24.0f, 0.0f};
    s.velocityRandom = {0.4f, 2.0f, 0.4f};
    s.windInfluence = 1.0f;
    s.collide = true; // stops on roofs, rocks, crates (anything with a collider)
    s.sizeStart = s.sizeEnd = 0.012f;
    s.sizeRandom = 0.2f;
    s.colorStart = s.colorMid = {0.75f, 0.8f, 0.85f, 0.35f};
    s.colorEnd = {0.75f, 0.8f, 0.85f, 0.2f};
    s.stretchWithVelocity = true;
    s.stretch = 0.03f;
    s.softness = 0.05f;
    s.rotationSpeed = 0.0f;

    // Sound layers: all loops, crossfaded by update() from the blended state.
    m_rainAudio = addLoop(rainObj, rainSound);
    m_stormAudio = addLoop(rainObj, stormSound);
    m_windAudio = addLoop(rainObj, windSound);
}

ve::AudioSource* Weather::addLoop(ve::GameObject& owner, const std::shared_ptr<ve::AudioClip>& clip)
{
    if (!clip) return nullptr;
    auto& src = owner.addComponent<ve::AudioSource>(clip);
    src.loop = true;
    src.spatial = false;
    src.bus = "Weather"; // AudioZones (indoors) muffle this bus
    src.volume = 0.0f;
    src.playOnStart = true;
    return &src;
}

Weather::RainParts Weather::rainParts() const
{
    RainParts parts;
    parts.object = m_rainId ? scene().findById(m_rainId) : nullptr;
    if (!parts.object) return parts;
    // A component pointer is only valid while the object still owns it. Compare
    // addresses against the live list (never dereference a possibly stale one).
    auto owned = [&](auto* ptr) -> decltype(ptr) {
        if (!ptr) return nullptr;
        for (const auto& c : parts.object->components())
            if (c.get() == ptr) return ptr;
        return nullptr;
    };
    parts.emitter = owned(m_rain);
    parts.rain = owned(m_rainAudio);
    parts.storm = owned(m_stormAudio);
    parts.wind = owned(m_windAudio);
    return parts;
}

void Weather::updateLightning(float dt)
{
    auto* composite = renderer().graph().findPass<ve::CompositePass>();
    if (!composite) return;
    auto rnd = [&] {
        m_rng = m_rng * 1664525u + 1013904223u;
        return static_cast<float>(m_rng >> 8) / 16777216.0f;
    };
    float flash = 0.0f;
    if (lightning && m_current.thunder > 0.05f) {
        m_nextFlash -= dt * m_current.thunder;
        if (m_nextFlash <= 0.0f && m_flashT < 0.0f) {
            m_flashT = 0.0f;
            m_nextFlash = lightningInterval * (0.3f + 1.4f * rnd());
        }
    }
    if (m_flashT >= 0.0f) {
        // A main strike and a couple of flickers over ~0.5 s.
        m_flashT += dt;
        const float t = m_flashT;
        auto pulse = [&](float at, float width, float amp) {
            const float x = (t - at) / width;
            return x < 0.0f ? 0.0f : amp * std::exp(-x * x * 4.0f) * (x < 1.5f ? 1.0f : 0.0f);
        };
        flash = std::max({pulse(0.0f, 0.06f, 1.0f), pulse(0.12f, 0.05f, 0.6f), pulse(0.28f, 0.12f, 0.8f)});
        if (t > 0.5f) m_flashT = -1.0f;
    }
    // Multiplies whatever exposure the game sets (eye adaptation, grading).
    composite->exposureMultiplier = 1.0f + flash * lightningStrength * m_current.thunder;
}

void Weather::update(float dt)
{
    m_t = std::min(1.0f, m_t + dt / m_duration);
    const float s = m_t * m_t * (3.0f - 2.0f * m_t);
    m_current = lerp(m_from, presets[size_t(m_target)], s);
    // Things get wet quickly in rain and dry slowly afterwards.
    const float rate = m_current.rain > m_wetness ? 0.15f : 0.02f;
    m_wetness += (m_current.rain - m_wetness) * std::min(1.0f, rate * dt);

    ve::Renderer& r = renderer();
    if (auto* atmo = r.graph().findPass<AtmospherePass>()) {
        atmo->coverage = m_current.coverage;
        atmo->cloudDensity = m_current.cloudDensity;
        atmo->haze = m_current.haze;
        atmo->overcast = std::clamp((m_current.coverage - 0.5f) * 2.0f, 0.0f, 1.0f);
    }
    m_gustPhase += dt;
    const float gust = 1.0f + 0.35f * std::sin(m_gustPhase * 0.7f) * std::sin(m_gustPhase * 1.9f + 1.0f);
    const glm::vec2 dir = glm::normalize(windDirection);
    r.setGlobalParam(SlotWind, glm::vec4(dir, m_current.wind * gust, gust));
    r.setGlobalParam(SlotWeather, glm::vec4(m_wetness, m_current.rain, m_current.coverage, m_current.fog));

    const RainParts parts = rainParts();
    if (parts.emitter) parts.emitter->settings.rate = m_current.rain * 2500.0f;
    // Crossfade the sound layers. With no storm sound, the rain loop covers storms too.
    const float storm = parts.storm ? m_current.thunder : 0.0f;
    auto setVolume = [](ve::AudioSource* src, float v) {
        if (!src) return;
        src->volume = v;
        src->applySettings();
    };
    setVolume(parts.rain, m_current.rain * (1.0f - storm) * rainVolume);
    setVolume(parts.storm, std::max(storm * stormVolume, stormBackground));
    setVolume(parts.wind, std::min(m_current.wind / std::max(windVolumeAt, 0.1f), 1.6f) * windVolume);

    // For ambience (AmbientLayer parameters) and game code: 0..1 each.
    audio().setParameter("rain", std::clamp(m_current.rain, 0.0f, 1.0f));
    audio().setParameter("storm", std::clamp(m_current.thunder, 0.0f, 1.0f));
    audio().setParameter("wind", std::clamp(m_current.wind / std::max(windVolumeAt, 0.1f), 0.0f, 1.0f));
    updateLightning(dt);
}

void Weather::onDestroy()
{
    if (ve::GameObject* o = m_rainId ? scene().findById(m_rainId) : nullptr) o->destroy();
    m_rainId = 0;
    m_rain = nullptr;
    m_rainAudio = m_stormAudio = m_windAudio = nullptr;
}

void Weather::lateUpdate(float)
{
    ve::GameObject* rainObject = m_rainId ? scene().findById(m_rainId) : nullptr;
    if (!rainObject) return;
    if (ve::Camera* cam = scene().mainCamera()) {
        const glm::vec3 p = cam->transform().position();
        const glm::vec2 dir = glm::normalize(windDirection);
        // Spawn upwind so drops blow into view.
        rainObject->transform().setPosition(p + glm::vec3(-dir.x, 0.0f, -dir.y) * m_current.wind * 0.6f +
                                            glm::vec3(0.0f, 12.0f, 0.0f));
    }
}

} // namespace vfx
