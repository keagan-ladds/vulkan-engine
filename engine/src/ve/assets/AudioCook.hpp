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

// Bump when the cooker's output changes: every cooked sound is redone.
constexpr uint32_t kAudioCookerVersion = 1;

struct AudioSettings {
    bool compress = true;
    float quality = 0.4f;
    bool mono = false;
    uint32_t sampleRate = 0;

    // <sound>.audio if there is one (fields it leaves out: defaults).
    static AudioSettings forFile(const std::filesystem::path& soundFile);
    static std::filesystem::path settingsFile(const std::filesystem::path& soundFile); // <sound>.audio
    nlohmann::json toJson() const;
    void applyJson(const nlohmann::json& j);
    uint64_t cookHash() const;
};

// Sound files the cooker handles (by extension: .wav, .flac).
bool isCookableAudio(const std::filesystem::path& file);
// <cacheDir>/<asset path>.ogg
std::filesystem::path cookedAudioPath(const std::filesystem::path& cacheDir, const std::string& assetPath);

struct AudioCookResult {
    bool ok = false;
    std::string error;
    uint32_t channels = 0, sampleRate = 0;
    double seconds = 0;     // length of the sound
    uint64_t bytes = 0;     // of the cooked file
    double milliseconds = 0;
};
// Cooks `source` into `output` (via a temporary file, then renamed).
AudioCookResult cookAudio(const std::filesystem::path& source, const AudioSettings& settings,
                          const std::filesystem::path& output, const std::atomic<bool>* cancelled = nullptr);

// Reads a cooked file's stamp and checks it (see Cook.hpp). `settings` null
// (no source to read them from): `settingsOut` gets the stored ones.
CookedState checkCookedAudio(const std::filesystem::path& cooked, const std::filesystem::path& source,
                             const AudioSettings* settings, AudioSettings* settingsOut = nullptr);

} // namespace ve
