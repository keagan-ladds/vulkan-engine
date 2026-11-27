#pragma once

// =============================================================================
// AudioCook - turns uncompressed sound files (WAV, FLAC) into Ogg Vorbis for
// the cook cache: typically 8-12x smaller, decoded by miniaudio while
// playing. Same scheme as textures (TextureCook.hpp): settings belong to the
// sound, cooked files carry a stamp (here in the Ogg file's Vorbis comment,
// tag VE_COOK) and are used while current.
//
// Settings: an optional "<sound>.audio" file next to it (fields it leaves out
// keep their defaults):
//
//   { "quality": 0.3, "mono": true }
//
//   compress    true   false: never cooked, the source file is used as is
//   quality     0.4    Vorbis VBR quality, -0.1 .. 1 (0.4 ~ 128 kbit/s stereo;
//                      0.2 is fine for ambience, 0.6+ for music you love)
//   mono        false  mix down to one channel (positional 3D sounds are
//                      played mono anyway: half the size)
//   sampleRate  0      resample to this rate (0 = keep), e.g. 22050 for noise
//
// MP3 and Ogg sources aren't cooked (already compressed: re-encoding only
// loses quality). Pure CPU work, safe on job threads and in vecook.
// =============================================================================

#include "ve/assets/Cook.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

namespace ve {

/// Bump when the cooker's output changes: every cooked sound is redone.
constexpr uint32_t kAudioCookerVersion = 1;

/// How a sound is cooked: from its `<sound>.audio` file (JSON), defaults otherwise.
struct AudioSettings {
    bool compress = true;    ///< Cook to Ogg Vorbis; false = ship and play the source file as it is.
    float quality = 0.4f;    ///< Vorbis quality, -0.1..1 (0.4 ~ 128 kbit/s stereo).
    bool mono = false;       ///< Mix down to one channel (positional sounds don't need stereo).
    uint32_t sampleRate = 0; ///< Resample to this rate in Hz; 0 = keep the source's.

    /// `<sound>`.audio if there is one (fields it leaves out: defaults).
    static AudioSettings forFile(const std::filesystem::path& soundFile);
    static std::filesystem::path settingsFile(const std::filesystem::path& soundFile); ///< `<sound>`.audio
    nlohmann::json toJson() const;            ///< The settings as a .audio file's JSON.
    void applyJson(const nlohmann::json& j);  ///< Sets the fields `j` has (others keep their values).
    uint64_t cookHash() const;                ///< Changes whenever the cooked result would (stamped in cooked files).
};

/// Sound files the cooker handles (by extension: .wav, .flac).
bool isCookableAudio(const std::filesystem::path& file);
/// `<cacheDir>`/`<asset path>`.ogg
std::filesystem::path cookedAudioPath(const std::filesystem::path& cacheDir, const std::string& assetPath);

/// What cookAudio did.
struct AudioCookResult {
    bool ok = false;          ///< The cooked file was written.
    std::string error;        ///< Why not (when !ok).
    uint32_t channels = 0;    ///< Channels in the cooked file.
    uint32_t sampleRate = 0;  ///< Its sample rate (Hz).
    double seconds = 0;     ///< length of the sound
    uint64_t bytes = 0;     ///< of the cooked file
    double milliseconds = 0;  ///< How long cooking took.
};
/// Cooks `source` into `output` (via a temporary file, then renamed).
/// Pure CPU work (safe on job threads); `cancelled` (optional) is polled.
AudioCookResult cookAudio(const std::filesystem::path& source, const AudioSettings& settings,
                          const std::filesystem::path& output, const std::atomic<bool>* cancelled = nullptr);

/// Reads a cooked file's stamp and checks it (see Cook.hpp). `settings` null
/// (no source to read them from): `settingsOut` gets the stored ones.
CookedState checkCookedAudio(const std::filesystem::path& cooked, const std::filesystem::path& source,
                             const AudioSettings* settings, AudioSettings* settingsOut = nullptr);

} // namespace ve
