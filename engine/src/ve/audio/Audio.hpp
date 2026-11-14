#pragma once

// =============================================================================
// Audio - 2D/3D sound playback built on miniaudio.
//
//   auto clip = assets().loadAudio("audio/fire.wav");
//   auto& src = go.addComponent<ve::AudioSource>(clip);
//   src.loop = true; src.playOnStart = true; src.maxDistance = 25.0f;   // 3D, follows go
//
//   audio().playOneShot(*assets().loadAudio("audio/pickup.wav"));      // fire & forget
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

class AudioSystem {
public:
    AudioSystem() = default;
    ~AudioSystem();
    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    void init();
    void shutdown();
    bool available() const { return m_engine != nullptr; }

    void setListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up);
    void setMasterVolume(float volume);
    float masterVolume() const { return m_masterVolume; }

    // Plays a clip once. With a position it's 3D, otherwise 2D (UI sounds).
    void playOneShot(const AudioClip& clip, float volume = 1.0f, std::optional<glm::vec3> position = std::nullopt,
                     float pitch = 1.0f);

    // Engine internal: frees finished one-shots.
    void update();
    ma_engine* engine() const { return m_engine; }

private:
    ma_engine* m_engine = nullptr;
    std::vector<ma_sound*> m_oneShots; // owned; freed in update()/shutdown()
    float m_masterVolume = 1.0f;
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

    void start() override;
    void lateUpdate(float dt) override;
    void onDestroy() override;

private:
    void release();
    ma_sound* m_sound = nullptr; // owned
    bool m_loaded = false;
};

} // namespace ve
