#pragma once

// =============================================================================
// Weather - a GameObject that blends between weather presets by driving the
// other effects: cloud coverage/density, fog amount, haze, wind, rain
// particles (following the camera) and an optional rain sound.
//
//   auto& weather = scene.spawn<vfx::Weather>();
//   weather.setWeather(vfx::Weather::Type::Rain, 20.0f);   // over 20 seconds
//   sun.intensity = baseSun * weather.sunScale();          // dim the sun when overcast
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
    enum class Type { Clear, Hazy, Overcast, Rain, Count };

    struct State {
        float coverage;    // cloud coverage
        float cloudDensity;
        float fog;         // fog density multiplier
        float haze;        // atmosphere aerosol amount
        float wind;        // wind strength (m/s)
        float rain;        // 0..1
        float sunScale;    // multiplier for direct sunlight
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
    std::shared_ptr<ve::AudioClip> rainSound;

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
};

} // namespace vfx
