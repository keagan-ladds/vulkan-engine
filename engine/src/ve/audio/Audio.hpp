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
// The listener (your ears) follows the main camera automatically.
// If no audio device is available the system disables itself silently.
// =============================================================================

#include "ve/scene/GameObject.hpp"

#include <glm/glm.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

struct ma_engine;
struct ma_sound;

namespace ve {

class AudioClip {
public:
    AudioClip(std::string path, bool stream) : m_path(std::move(path)), m_stream(stream) {}
    const std::string& path() const { return m_path; }
    bool stream() const { return m_stream; }

private:
    std::string m_path;
    bool m_stream;
};

class AudioZone;
struct AudioFilter; // miniaudio low-pass node (internal)

// A mixer bus: sounds -> low-pass -> output. Volume/lowpass are the game's
// settings; zones apply on top.
struct AudioBus {
    std::string name;
    float volume = 1.0f;
    float lowpass = 20000.0f; // Hz (20000 = no filtering)
    // Internal: current zone effect (smoothed) and the miniaudio nodes.
    float zoneVolume = 1.0f;
    float zoneLowpass = 20000.0f;
    float appliedLowpass = -1.0f;
    ma_sound* group = nullptr;
    AudioFilter* filter = nullptr;
};

class AudioSystem {
public:
    static constexpr float kNoFilter = 20000.0f;

    AudioSystem() = default;
    ~AudioSystem();
    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    void init();
    void shutdown();
    bool available() const { return m_engine != nullptr; }

    void setListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up);
    const glm::vec3& listenerPosition() const { return m_listener; }
    void setMasterVolume(float volume);
    float masterVolume() const { return m_masterVolume; }

    // Buses. bus() creates the bus on first use.
    AudioBus& bus(const std::string& name);
    void setBusVolume(const std::string& name, float volume) { bus(name).volume = volume; }
    void setBusLowpass(const std::string& name, float hz) { bus(name).lowpass = hz; }
    const std::vector<std::unique_ptr<AudioBus>>& buses() const { return m_buses; }

    // Plays a clip once. With a position it's 3D, otherwise 2D (UI sounds).
    void playOneShot(const AudioClip& clip, float volume = 1.0f, std::optional<glm::vec3> position = std::nullopt,
                     float pitch = 1.0f, const std::string& bus = "SFX");

    // Engine internal: frees finished one-shots, applies zones to buses.
    void update(float dt);
    ma_engine* engine() const { return m_engine; }
    void addZone(AudioZone* zone) { m_zones.push_back(zone); }
    void removeZone(AudioZone* zone) { std::erase(m_zones, zone); }
    void onGui(); // bus volumes + active zones

private:
    ma_engine* m_engine = nullptr;
    std::vector<ma_sound*> m_oneShots; // owned; freed in update()/shutdown()
    std::vector<std::unique_ptr<AudioBus>> m_buses;
    std::vector<AudioZone*> m_zones;
    glm::vec3 m_listener{0.0f};
    float m_masterVolume = 1.0f;
};

// Changes buses (volume, low-pass) while the listener is inside its shape.
// The shape is in the GameObject's local space (position, rotation and scale
// apply); a Cylinder is vertical with its base at the object's origin.
// Overlapping zones combine by taking the strongest effect per bus.
class AudioZone : public Component {
public:
    enum class Shape { Box, Sphere, Cylinder };
    struct Effect {
        std::string bus;
        float volume = 1.0f;
        float lowpass = AudioSystem::kNoFilter; // Hz
    };
    Shape shape = Shape::Box;
    glm::vec3 halfExtents{2.0f}; // Box (centred on the origin)
    float radius = 2.0f;         // Sphere, Cylinder
    float height = 3.0f;         // Cylinder
    std::vector<Effect> effects;
    float fadeTime = 0.35f;      // seconds to blend in/out when crossing the boundary

    bool contains(const glm::vec3& worldPoint) const;
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

class AudioSource : public Component {
public:
    explicit AudioSource(std::shared_ptr<AudioClip> clip = nullptr) : clip(std::move(clip)) {}
    ~AudioSource() override;

    void play();
    void stop();
    bool isPlaying() const;
    // Apply volume/pitch/distance changes made after play().
    void applySettings();

    std::shared_ptr<AudioClip> clip;
    float volume = 1.0f;
    float pitch = 1.0f;
    bool loop = false;
    bool spatial = true;      // 3D positional
    bool playOnStart = false;
    float minDistance = 1.0f; // full volume within this distance
    float maxDistance = 40.0f;
    std::string bus = "SFX";  // mixer bus (set before play)
    // Occlusion (3D sources): a few physics rays per second from the listener
    // to the source; when blocked (terrain, buildings, anything with a
    // collider) the sound is low-passed and quieter. Set before play().
    bool occlusion = false;
    float occlusionVolume = 0.45f;   // volume multiplier when fully blocked
    float occlusionLowpass = 700.0f; // Hz when fully blocked
    float occluded() const { return m_occluded; } // 0..1, smoothed

    void start() override;
    void lateUpdate(float dt) override;
    void onDestroy() override;

private:
    void release();
    void updateOcclusion(float dt);
    ma_sound* m_sound = nullptr; // owned
    AudioFilter* m_filter = nullptr; // occlusion low-pass (owned)
    bool m_loaded = false;
    float m_occluded = 0.0f, m_occlusionTarget = 0.0f, m_rayTimer = 0.0f;
    float m_appliedLowpass = -1.0f;
};

} // namespace ve
