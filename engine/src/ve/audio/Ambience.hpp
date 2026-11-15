#pragma once

// =============================================================================
// AmbientLayer - one layer of ambience (crickets at night, birds at dawn,
// frogs in the swamp): a looping bed and/or random one-shots around the
// listener, faded in and out by time of day, biome and parameters.
//
//   Ambience (GameObject)
//     Crickets     [AmbientLayer] loop = crickets.wav, hours 19..5.5, biomes "Savannah:1, Camp:0.6"
//     Dawn birds   [AmbientLayer] oneShots = "bird1.wav; bird2.wav", hours 5..9, parameters "rain:-1"
//     Swamp frogs  [AmbientLayer] loop = frogs.wav, hours 18..7, biomes "Swamp:1"
//
// Level = time x biome x parameters, smoothed over fadeTime:
//   - time: 1 between fromHour and toHour (wrapping past midnight), blended
//     over hourFade hours at each end. Reads the "timeOfDay" parameter (set
//     by the effects module's DayNightCycle); always 1 without it.
//   - biome: the Terrain's biome weights around the listener (averaged over
//     biomeRadius, so borders crossfade), same syntax as vegetation:
//     "Savannah:1, Swamp:-1"; empty = everywhere.
//   - parameters: "rain:-1, wind:-0.5" = 1 + sum(weight x value), clamped to
//     0..1; "*" sets the constant instead of 1 ("*:0, danger:1" = only with danger).
// One-shots fire less often at lower levels; they're 3D (between distance.x
// and distance.y from the listener) unless distance.y is 0.
// Plays on the "Ambient" bus by default, so AudioZones (huts) muffle it.
// =============================================================================

#include "ve/scene/GameObject.hpp"

#include <glm/glm.hpp>

#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

struct ma_sound;

namespace ve {

class AudioClip;

class AmbientLayer : public Component {
public:
    // ---- Sounds ----
    std::shared_ptr<AudioClip> loop; // looping bed (2D), streamed
    std::string oneShots;            // "audio/bird1.wav; audio/bird2.wav"
    glm::vec2 interval{3.0f, 10.0f}; // s between one-shots at full level
    glm::vec2 distance{10.0f, 35.0f};// m from the listener (y = 0: 2D)
    float heightRange = 4.0f;        // one-shots: 0..this above the listener
    float pitchVariation = 0.08f;
    float volumeVariation = 0.3f;
    float volume = 1.0f;
    std::string bus = "Ambient";

    // ---- When / where ----
    float fromHour = 0.0f, toHour = 24.0f; // 0..24 = always
    float hourFade = 0.75f;
    std::string biomes;
    float biomeRadius = 12.0f;
    std::string parameters;
    float fadeTime = 3.0f; // s to follow changes

    float level() const { return m_level; }       // current 0..1
    float targetLevel() const { return m_target; }

    void start() override;
    void update(float dt) override;
    void onDestroy() override;
    ~AmbientLayer() override;

    // The time-of-day factor alone (exposed for tests and tools).
    static float hourWeight(float hour, float from, float to, float fade);

private:
    float computeTarget();
    void releaseLoop();

    ma_sound* m_loop = nullptr;
    std::string m_loopPath;
    std::vector<std::shared_ptr<AudioClip>> m_shots;
    std::string m_shotsSpec;
    std::vector<std::pair<int, float>> m_biomeWeights;
    std::string m_biomeKey;
    float m_level = 0.0f, m_target = 0.0f;
    float m_shotTimer = -1.0f;
    std::mt19937 m_rng;
};

} // namespace ve
