#pragma once

// =============================================================================
// Audio - 2D/3D sound playback built on miniaudio, with mixer buses, muffling
// zones and occlusion.
//
//   auto clip = assets().loadAudio("audio/fire.wav");
//   auto& src = go.addComponent<ve::AudioSource>(clip);
//   src.loop = true; src.playOnStart = true; src.maxDistance = 25.0f;   // 3D, follows go
//   src.occlusion = true;          // muffled when terrain/buildings block the way to the listener
//
//   audio().playOneShot(*assets().loadAudio("audio/pickup.wav"));      // fire & forget
//
// Buses: every sound plays through a named bus (AudioSource::bus, default
// "SFX"; built in: SFX, Ambient, Weather, Music, UI). Each bus has a volume and
// a low-pass filter: audio().setBusVolume("Music", 0.5f) for an options menu.
//
// Zones: an AudioZone (box / sphere / vertical cylinder) changes buses while
// the listener is inside, e.g. inside a hut:
//   auto& zone = hut.addComponent<ve::AudioZone>();
//   zone.shape = ve::AudioZone::Shape::Cylinder; zone.radius = 2.5f; zone.height = 3.0f;
//   zone.effects = {{"Weather", 0.35f, 500.0f}, {"Ambient", 0.6f, 1500.0f}}; // volume, low-pass Hz
//
// Music: audio().music() plays moods (playlists) with crossfades; see Music.hpp.
// Ambience: AmbientLayer components (loops / random one-shots by time of day,
// biome and parameters); see Ambience.hpp.
//
// Parameters: named floats that ambience (and game code) reacts to. The
// effects module sets "timeOfDay" (hours, AudioSystem::kTimeOfDay), "night" (0..1), "rain", "wind",
// "storm"; the game can add its own ("danger"...):
//   audio().setParameter("danger", 1.0f);
//
// The listener (your ears) follows the main camera automatically.
// If no audio device is available the system disables itself silently.
// =============================================================================

#include "ve/scene/GameObject.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct ma_engine;
struct ma_sound;

namespace ve {

/// A sound file to play: where it is and whether to stream it. Get one from
/// AssetManager::loadAudio (which picks the cooked copy); cheap to share.
class AudioClip {
public:
    /// @param path the file to open (on disk or in a mounted archive).
    /// @param stream decode while playing (music, long ambiences) instead of loading it all into memory.
    /// @param assetPath the asset path scenes save (defaults to `path`).
    AudioClip(std::string path, bool stream, std::string assetPath = {})
        : m_path(std::move(path)), m_assetPath(std::move(assetPath)), m_stream(stream)
    {
        if (m_assetPath.empty()) m_assetPath = m_path;
    }
    const std::string& path() const { return m_path; }           ///< file on disk
    const std::string& assetPath() const { return m_assetPath; } ///< as saved in scenes ("audio/fire.wav")
    /// Whether it is decoded while playing rather than loaded whole.
    bool stream() const { return m_stream; }

private:
    std::string m_path;
    std::string m_assetPath;
    bool m_stream;
};

class AudioZone;
class MusicPlayer;
struct AudioFilter; ///< miniaudio low-pass node (internal)

/// Engine internal: owning handles for miniaudio objects (uninit, then delete),
/// so every exit path frees them. The deleters live in Audio.cpp (the types are
/// incomplete here). SoundPtr also holds sound groups (a group is an ma_sound).
struct SoundDeleter {
    /// Uninitialises and frees a sound.
    void operator()(ma_sound* s) const;
};
/// Frees a bus low-pass node.
struct FilterDeleter {
    /// Uninitialises and frees the filter.
    void operator()(AudioFilter* f) const;
};
/// Frees the miniaudio engine.
struct EngineDeleter {
    /// Uninitialises and frees the engine.
    void operator()(ma_engine* e) const;
};
using SoundPtr = std::unique_ptr<ma_sound, SoundDeleter>;
using FilterPtr = std::unique_ptr<AudioFilter, FilterDeleter>;
/// A sound from a file (MA_SOUND_FLAG_* flags) playing into `group` (null: the
/// engine's output); null if it can't be loaded.
SoundPtr openSound(ma_engine* engine, const std::string& file, uint32_t flags, ma_sound* group);

/// A mixer bus: sounds -> low-pass -> output. Volume/lowpass are the game's
/// settings; zones apply on top.
struct AudioBus {
    std::string name;  ///< The bus's name ("SFX", "Music", "Ambient", ...).
    float volume = 1.0f;  ///< The game's volume for the bus (0..1).
    float lowpass = 20000.0f; ///< Hz (20000 = no filtering)
    /// Internal: current zone effect (smoothed) and the miniaudio nodes.
    float zoneVolume = 1.0f;
    float zoneLowpass = 20000.0f;  ///< Internal: zones' low-pass for this bus (Hz, smoothed).
    float appliedLowpass = -1.0f;  ///< Internal: the cutoff last applied to the filter node.
    FilterPtr filter;  ///< Internal: the bus's low-pass node.
    SoundPtr group; ///< (declared last: freed first, before the filter it feeds)
};

/// Optional settings for AudioSystem::playOneShot.
struct OneShotSettings {
    float volume = 1.0f;  ///< Linear volume (0..1+).
    std::optional<glm::vec3> position; ///< 3D if set, else 2D
    float pitch = 1.0f;  ///< Playback rate: 1 = normal, 2 = an octave up (and twice as fast).
    std::string bus = "SFX";  ///< Mixer bus to play on.
    float minDistance = 1.0f;          ///< 3D: full volume within this distance...
    float maxDistance = 0.0f;          ///< ...fading linearly to silence here (0 = miniaudio's default roll-off)
};

/// Sound output (miniaudio): one-shots, mixer buses, the 3D listener, audio
/// zones, named parameters and music. Engine::audio(); main thread only.
class AudioSystem {
public:
    /// A low-pass cutoff (Hz) that means "no filtering".
    static constexpr float kNoFilter = 20000.0f;
    /// The parameter holding the time of day in hours (0..24). Set by the effects
    /// module's DayNightCycle; AmbientLayer::fromHour/toHour read it.
    static constexpr const char* kTimeOfDay = "timeOfDay";

    AudioSystem();
    ~AudioSystem();
    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    /// Opens the audio device (Engine: unless EngineConfig::enableAudio is false). Without a device, playing is a silent no-op.
    void init();
    /// Stops everything and closes the device.
    void shutdown();
    /// Whether an audio device is open (sounds are audible).
    bool available() const { return m_engine != nullptr; }

    /// Places the 3D listener (the Engine does it from the main camera each frame).
    void setListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up);
    /// Where the 3D listener is (world space).
    const glm::vec3& listenerPosition() const { return m_listener; }
    /// Overall output volume (0..1).
    void setMasterVolume(float volume);
    /// The overall output volume.
    float masterVolume() const { return m_masterVolume; }

    /// Buses. bus() creates the bus on first use.
    AudioBus& bus(const std::string& name);
    /// The game's volume for a bus (0..1); zones apply on top.
    void setBusVolume(const std::string& name, float volume) { bus(name).volume = volume; }
    /// The game's low-pass cutoff for a bus in Hz (kNoFilter = off), e.g. a muffled pause menu.
    void setBusLowpass(const std::string& name, float hz) { bus(name).lowpass = hz; }
    /// Every bus created so far.
    const std::vector<std::unique_ptr<AudioBus>>& buses() const { return m_buses; }

    /// Plays a clip once, fire and forget.
    /// @param volume linear volume (0..1+).
    /// @param position world position for a 3D sound; none = 2D (UI sounds).
    /// @param pitch playback rate (1 = normal).
    /// @param bus mixer bus to play on.
    void playOneShot(const AudioClip& clip, float volume = 1.0f, std::optional<glm::vec3> position = std::nullopt,
                     float pitch = 1.0f, const std::string& bus = "SFX");
    /// Plays a clip once with the settings in OneShotSettings (3D when it has a position).
    void playOneShot(const AudioClip& clip, const OneShotSettings& settings);

    /// Music moods, playlists and crossfades (Music.hpp).
    MusicPlayer& music() { return *m_music; }

    /// Named parameters (see the top of this file). Unset parameters read as `fallback`.
    void setParameter(const std::string& name, float value) { m_parameters[name] = value; }
    /// A named parameter's value (setParameter), or `fallback` if it was never set.
    float parameter(const std::string& name, float fallback = 0.0f) const;
    /// Whether the parameter has been set.
    bool hasParameter(const std::string& name) const { return m_parameters.count(name) != 0; }
    /// Every parameter set so far.
    const std::map<std::string, float>& parameters() const { return m_parameters; }

    /// Engine internal: frees finished one-shots, applies zones to buses.
    void update(float dt);
    /// The miniaudio engine (null without a device): for code that plays sounds itself.
    ma_engine* engine() const { return m_engine.get(); }
    /// AudioZone: registers itself.
    void addZone(AudioZone* zone) { m_zones.push_back(zone); }
    /// AudioZone: unregisters itself.
    void removeZone(AudioZone* zone) { std::erase(m_zones, zone); }
    void onGui(); ///< bus volumes, zones, parameters, music

private:
    std::unique_ptr<ma_engine, EngineDeleter> m_engine;
    std::vector<SoundPtr> m_oneShots; ///< freed in update() once done, or in shutdown()
    std::vector<std::unique_ptr<AudioBus>> m_buses;
    std::vector<AudioZone*> m_zones;
    std::map<std::string, float> m_parameters;
    std::unique_ptr<MusicPlayer> m_music;
    glm::vec3 m_listener{0.0f};
    float m_masterVolume = 1.0f;
};

/// Changes buses (volume, low-pass) while the listener is inside its shape.
/// The shape is in the GameObject's local space (position, rotation and scale
/// apply); a Cylinder is vertical with its base at the object's origin.
/// Overlapping zones combine by taking the strongest effect per bus.
class AudioZone : public Component {
public:
    /// The zone's shape (in the object's local space).
    enum class Shape { Box, Sphere, Cylinder };
    /// What the zone does to one bus while the listener is inside.
    struct Effect {
        std::string bus;  ///< The bus affected ("Ambient", "SFX"...).
        float volume = 1.0f;  ///< Volume multiplier for the bus (0..1).
        float lowpass = AudioSystem::kNoFilter; ///< Hz
    };
    Shape shape = Shape::Box;  ///< Box, Sphere or Cylinder.
    glm::vec3 halfExtents{2.0f}; ///< Box (centred on the origin)
    float radius = 2.0f;         ///< Sphere, Cylinder
    float height = 3.0f;         ///< Cylinder
    std::vector<Effect> effects;  ///< One per bus to change.
    float fadeTime = 0.35f;      ///< seconds to blend in/out when crossing the boundary

    /// Whether a world-space point is inside the shape.
    bool contains(const glm::vec3& worldPoint) const;
    /// Whether the listener is inside (effects are fading in or applied).
    bool listenerInside() const { return m_inside; }

    void start() override;
    void onDestroy() override;
    ~AudioZone() override;

private:
    friend class AudioSystem;
    void unregister();
    bool m_inside = false;
    bool m_registered = false;
};

/// Plays a clip from its object: positional (3D) by default, following the
/// object, optionally looping and occluded by geometry.
class AudioSource : public Component {
public:
    /// A source for `clip` (or set `clip` later).
    explicit AudioSource(std::shared_ptr<AudioClip> clip = nullptr) : clip(std::move(clip)) {}
    ~AudioSource() override;

    /// Starts playing `clip` from the beginning (stops what was playing).
    void play();
    /// Stops playback.
    void stop();
    /// Whether it is playing right now.
    bool isPlaying() const;
    /// Apply volume/pitch/distance changes made after play().
    void applySettings();

    std::shared_ptr<AudioClip> clip;  ///< What to play.
    float volume = 1.0f;  ///< Linear volume (0..1+).
    float pitch = 1.0f;  ///< Playback rate: 1 = normal.
    bool loop = false;  ///< Repeat until stopped.
    bool spatial = true;      ///< 3D positional
    bool playOnStart = false;  ///< Start playing when the component starts.
    float minDistance = 1.0f; ///< full volume within this distance
    float maxDistance = 40.0f;  ///< Distance (m) at which it has faded to silence (linear roll-off from minDistance).
    std::string bus = "SFX";  ///< mixer bus (set before play)
    /// Occlusion (3D sources): a few physics rays per second from the listener
    /// to the source; when blocked (terrain, buildings, anything with a
    /// collider) the sound is low-passed and quieter. Set before play().
    bool occlusion = false;
    float occlusionVolume = 0.45f;   ///< volume multiplier when fully blocked
    float occlusionLowpass = 700.0f; ///< Hz when fully blocked
    float occluded() const { return m_occluded; } ///< 0..1, smoothed

    void start() override;
    void lateUpdate(float dt) override;
    void onDestroy() override;

private:
    void release();
    void updateOcclusion(float dt);
    SoundPtr m_sound;   ///< null until play() loaded it
    FilterPtr m_filter; ///< occlusion low-pass
    float m_occluded = 0.0f, m_occlusionTarget = 0.0f, m_rayTimer = 0.0f;
    float m_appliedLowpass = -1.0f;
};

class TypeRegistry;
/// Reflection (scene files, editor): this module's components. The Engine calls it at startup.
void registerAudioTypes(TypeRegistry& r);

} // namespace ve
