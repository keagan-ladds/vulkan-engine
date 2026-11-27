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
#include <ve/scene/ObjectRef.hpp>

#include <glm/glm.hpp>

#include <array>
#include <memory>

namespace ve {
class AudioClip;
class AudioSource;
} // namespace ve

namespace vfx {

class ParticleEmitter;

/// Blends between weather presets by driving clouds, fog, haze, wind, rain,
/// lightning and sounds (see the top of this file).
class Weather : public ve::GameObject {
public:
    /// The presets (Count = how many).
    enum class Type { Clear, Hazy, Overcast, Rain, Thunderstorm, Count };

    /// One preset's values (also the blended current state).
    struct State {
        float coverage;    ///< cloud coverage
        float cloudDensity; ///< cloud opacity multiplier
        float fog;         ///< fog density multiplier
        float haze;        ///< atmosphere aerosol amount
        float wind;        ///< wind strength (m/s)
        float rain;        ///< 0..1
        float sunScale;    ///< multiplier for direct sunlight
        float thunder;     ///< 0..1: storm sound + lightning
    };

    Weather();

    /// Blends to `type` over `transitionSeconds`.
    void setWeather(Type type, float transitionSeconds = 10.0f);
    /// The weather being blended to (or reached).
    Type weather() const { return m_target; }
    /// A preset's display name ("Thunderstorm").
    static const char* name(Type type);
    /// Blends to the next preset (cycling).
    void next(float transitionSeconds = 10.0f);

    /// The blended state right now.
    const State& current() const { return m_current; }
    /// Multiplier for direct sunlight right now (dim the sun's intensity with it).
    float sunScale() const { return m_current.sunScale; }
    /// How wet surfaces are (0..1; rises with rain, dries slowly afterwards).
    float wetness() const { return m_wetness; }

    std::array<State, static_cast<size_t>(Type::Count)> presets; ///< Values of each preset (editable).
    glm::vec2 windDirection{0.8f, 0.6f};                         ///< Horizontal wind direction (x, z).
    Type startWeather = Type::Clear;           ///< applied on start (unless setWeather was called first)
    std::shared_ptr<ve::AudioClip> rainSound;  ///< steady rain loop
    std::shared_ptr<ve::AudioClip> stormSound; ///< heavy rain + thunder loop (Thunderstorm)
    std::shared_ptr<ve::AudioClip> windSound;  ///< wind loop, volume follows wind strength
    float rainVolume = 0.6f;   ///< Volume of rainSound at full rain.
    float stormVolume = 0.8f;  ///< Volume of stormSound at full thunder.
    float windVolume = 0.35f;  ///< Volume of windSound at full wind.
    float stormBackground = 0.0f;              ///< storm loop never drops below this (distant-storm ambience)
    float windVolumeAt = 9.0f;                 ///< wind strength (m/s) that plays windVolume

    /// Lightning: random flashes (exposure pulses) while thunder > 0.
    bool lightning = true;
    float lightningInterval = 9.0f;            ///< average seconds between flashes at full storm
    float lightningStrength = 3.0f;            ///< peak exposure boost (x)

    void start() override;
    void update(float dt) override;
    void lateUpdate(float dt) override;
    void onGui() override {}
    void onDestroy() override; ///< removes the rain object it created

private:
    State m_from{}, m_current{};
    Type m_target = Type::Clear;
    bool m_explicit = false;
    float m_t = 1.0f, m_duration = 1.0f;
    float m_wetness = 0.0f;
    float m_gustPhase = 0.0f;
    /// The spawned "Rain" object and its components (the editor may delete
    /// either; the refs then resolve to null).
    ve::ObjectRef<> m_rainObject;
    ve::ComponentRef<ParticleEmitter> m_rain;
    ve::ComponentRef<ve::AudioSource> m_rainAudio, m_stormAudio, m_windAudio;
    ve::AudioSource* addLoop(ve::GameObject& owner, const std::shared_ptr<ve::AudioClip>& clip);
    void updateLightning(float dt);
    float m_nextFlash = 4.0f, m_flashT = -1.0f;
    uint32_t m_rng = 0x2468ACEu;
};

} // namespace vfx
