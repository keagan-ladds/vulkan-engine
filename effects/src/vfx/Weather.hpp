#pragma once

// =============================================================================
// Weather - a GameObject that blends between weather presets by driving the
// other effects: cloud coverage/density, fog amount, haze, wind, rain
// particles (following the camera), lightning flashes and ambient sound.
//
//   auto& weather = scene.spawn<vfx::Weather>();
//   weather.rainSound  = assets.loadAudio("audio/rain_loop.wav");
//   weather.stormSound = assets.loadAudio("audio/rain_thunder_loop.wav", /*stream*/ true);
//   weather.windSound  = assets.loadAudio("audio/wind_loop.wav");
//   weather.setWeather(vfx::Weather::Type::Thunderstorm, 20.0f);   // over 20 seconds
//   sun.intensity = baseSun * weather.sunScale();          // dim the sun when overcast
//
// Sounds are loops on the "Weather" audio bus (so AudioZones muffle them
// indoors). Their volumes follow the blended state (rain * (1 - thunder),
// thunder, wind), so changing weather crossfades them over the transition.
//
// It publishes wind (SlotWind) and wetness/rain (SlotWeather) as global
// shader params so materials (grass sway, wet surfaces) can react.
// =============================================================================

#include <ve/scene/GameObject.hpp>

#include <glm/glm.hpp>

#include <array>
#include <memory>

namespace ve {
class AudioClip;
class AudioSource;
} // namespace ve

namespace vfx {

class ParticleEmitter;

class Weather : public ve::GameObject {
public:
    enum class Type { Clear, Hazy, Overcast, Rain, Thunderstorm, Count };

    struct State {
        float coverage;    // cloud coverage
        float cloudDensity;
        float fog;         // fog density multiplier
        float haze;        // atmosphere aerosol amount
        float wind;        // wind strength (m/s)
        float rain;        // 0..1
        float sunScale;    // multiplier for direct sunlight
        float thunder;     // 0..1: storm sound + lightning
    };

    Weather();

    void setWeather(Type type, float transitionSeconds = 10.0f);
    Type weather() const { return m_target; }
    static const char* name(Type type);
    void next(float transitionSeconds = 10.0f);

    const State& current() const { return m_current; }
    float sunScale() const { return m_current.sunScale; }
    float wetness() const { return m_wetness; }

    std::array<State, static_cast<size_t>(Type::Count)> presets;
    glm::vec2 windDirection{0.8f, 0.6f};
    std::shared_ptr<ve::AudioClip> rainSound;  // steady rain loop
    std::shared_ptr<ve::AudioClip> stormSound; // heavy rain + thunder loop (Thunderstorm)
    std::shared_ptr<ve::AudioClip> windSound;  // wind loop, volume follows wind strength
    float rainVolume = 0.6f, stormVolume = 0.8f, windVolume = 0.35f;
    float stormBackground = 0.0f;              // storm loop never drops below this (distant-storm ambience)
    float windVolumeAt = 9.0f;                 // wind strength (m/s) that plays windVolume

    // Lightning: random flashes (exposure pulses) while thunder > 0.
    bool lightning = true;
    float lightningInterval = 9.0f;            // average seconds between flashes at full storm
    float lightningStrength = 3.0f;            // peak exposure boost (x)

    void start() override;
    void update(float dt) override;
    void lateUpdate(float dt) override;
    void onGui() override {}

private:
    State m_from{}, m_current{};
    Type m_target = Type::Clear;
    float m_t = 1.0f, m_duration = 1.0f;
    float m_wetness = 0.0f;
    float m_gustPhase = 0.0f;
    ve::GameObject* m_rainObject = nullptr;
    ParticleEmitter* m_rain = nullptr;
    ve::AudioSource* m_rainAudio = nullptr;
    ve::AudioSource* m_stormAudio = nullptr;
    ve::AudioSource* m_windAudio = nullptr;
    ve::AudioSource* addLoop(ve::GameObject& owner, const std::shared_ptr<ve::AudioClip>& clip);
    void updateLightning(float dt);
    float m_nextFlash = 4.0f, m_flashT = -1.0f;
    uint32_t m_rng = 0x2468ACEu;
};

} // namespace vfx
