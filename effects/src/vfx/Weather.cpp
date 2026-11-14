#include "vfx/Weather.hpp"

#include "vfx/Atmosphere.hpp"
#include "vfx/Particles.hpp"
#include "vfx/Vfx.hpp"

#include <ve/audio/Audio.hpp>
#include <ve/render/Renderer.hpp>
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
            m(a.wind, b.wind),         m(a.rain, b.rain),                 m(a.sunScale, b.sunScale)};
}
} // namespace

Weather::Weather() : GameObject("Weather")
{
    //                         coverage density fog  haze wind rain sun
    presets[size_t(Type::Clear)] = {0.30f, 0.8f, 0.7f, 1.0f, 2.5f, 0.0f, 1.00f};
    presets[size_t(Type::Hazy)] = {0.45f, 1.0f, 1.3f, 2.2f, 4.0f, 0.0f, 0.85f};
    presets[size_t(Type::Overcast)] = {0.85f, 1.3f, 1.8f, 2.0f, 7.0f, 0.0f, 0.30f};
    presets[size_t(Type::Rain)] = {0.97f, 1.8f, 1.8f, 2.0f, 9.0f, 1.0f, 0.15f};
    m_current = m_from = presets[size_t(Type::Clear)];
}

const char* Weather::name(Type type)
{
    switch (type) {
    case Type::Clear: return "Clear";
    case Type::Hazy: return "Hazy";
    case Type::Overcast: return "Overcast";
    case Type::Rain: return "Rain";
    default: return "?";
    }
}

void Weather::setWeather(Type type, float transitionSeconds)
{
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
    // Rain is a particle emitter that follows the camera around.
    auto& rainObj = scene().spawn("Rain");
    m_rainObject = &rainObj;
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
    s.sizeStart = s.sizeEnd = 0.012f;
    s.sizeRandom = 0.2f;
    s.colorStart = s.colorMid = {0.75f, 0.8f, 0.85f, 0.35f};
    s.colorEnd = {0.75f, 0.8f, 0.85f, 0.2f};
    s.stretchWithVelocity = true;
    s.stretch = 0.03f;
    s.softness = 0.05f;
    s.rotationSpeed = 0.0f;

    if (rainSound) {
        m_rainAudio = &rainObj.addComponent<ve::AudioSource>(rainSound);
        m_rainAudio->loop = true;
        m_rainAudio->spatial = false;
        m_rainAudio->volume = 0.0f;
        m_rainAudio->playOnStart = true;
    }
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

    if (m_rain) m_rain->settings.rate = m_current.rain * 2500.0f;
    if (m_rainAudio) {
        m_rainAudio->volume = m_current.rain * 0.6f;
        m_rainAudio->applySettings();
    }
}

void Weather::lateUpdate(float)
{
    if (!m_rainObject) return;
    if (ve::Camera* cam = scene().mainCamera()) {
        const glm::vec3 p = cam->transform().position();
        const glm::vec2 dir = glm::normalize(windDirection);
        // Spawn upwind so drops blow into view.
        m_rainObject->transform().setPosition(p + glm::vec3(-dir.x, 0.0f, -dir.y) * m_current.wind * 0.6f +
                                              glm::vec3(0.0f, 12.0f, 0.0f));
    }
}

} // namespace vfx
