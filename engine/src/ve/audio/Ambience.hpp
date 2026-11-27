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

#include "ve/audio/Audio.hpp" // SoundPtr
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

/// One ambience layer: a looping bed and / or random one-shots, faded by time
/// of day, biome and audio parameters (see the top of this file).
class AmbientLayer : public Component {
public:
    // ---- Sounds ----
    std::shared_ptr<AudioClip> loop; ///< looping bed (2D), streamed
    std::string oneShots;            ///< "audio/bird1.wav; audio/bird2.wav"
    glm::vec2 interval{3.0f, 10.0f}; ///< s between one-shots at full level
    glm::vec2 distance{10.0f, 35.0f};///< m from the listener (y = 0: 2D)
    float heightRange = 4.0f;        ///< one-shots: 0..this above the listener
    float pitchVariation = 0.08f;    ///< One-shots: random pitch change, +- this fraction (0.08 = +-8 %).
    float volumeVariation = 0.3f;    ///< One-shots: random volume reduction, up to this fraction.
    float volume = 1.0f;             ///< Linear volume of the layer at full level.
    std::string bus = "Ambient";      ///< Mixer bus (AudioZones muffle "Ambient" indoors).

    // ---- When / where ----
    float fromHour = 0.0f;           ///< Hour (0..24) the layer starts; may be > toHour (wraps past midnight).
    float toHour = 24.0f;            ///< Hour (0..24) it ends; 0..24 = always.
    float hourFade = 0.75f;          ///< Hours to fade in / out at each end.
    std::string biomes;              ///< Biome weights, "Savannah:1, Swamp:-1"; empty = everywhere.
    float biomeRadius = 12.0f;       ///< Metres around the listener the biome weights are averaged over.
    std::string parameters;          ///< Parameter weights, "rain:-1, wind:-0.5" (see the top of this file).
    float fadeTime = 3.0f; ///< s to follow changes

    float level() const { return m_level; }       ///< current 0..1
    float targetLevel() const { return m_target; } ///< The level it is fading towards (0..1).

    void start() override;
    void update(float dt) override;
    void onDestroy() override;
    ~AmbientLayer() override;

    /// The time-of-day factor alone (exposed for tests and tools).
    static float hourWeight(float hour, float from, float to, float fade);

private:
    float computeTarget();
    void releaseLoop();

    SoundPtr m_loop;
    std::string m_loopPath;
    std::string m_failedLoop; ///< a loop file that wouldn't open: not retried every frame
    std::vector<std::shared_ptr<AudioClip>> m_shots;
    std::string m_shotsSpec;
    std::vector<std::pair<int, float>> m_biomeWeights;
    std::string m_biomeSpec;    ///< `biomes` that m_biomeWeights was parsed from...
    uint64_t m_biomeVersion = 0; ///< ...for this terrain version
    std::vector<std::pair<std::string, float>> m_params; ///< parsed `parameters`
    std::string m_paramsSpec;
    float m_level = 0.0f, m_target = 0.0f;
    float m_shotTimer = -1.0f;
    std::mt19937 m_rng;
};

} // namespace ve
