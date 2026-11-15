#pragma once

// =============================================================================
// Music - moods (playlists) with crossfades, a mood stack and stingers.
//
// A mood is a playlist: tracks, shuffle, a silent gap between tracks, a volume
// and a fade time. Exactly one mood plays at a time: the base mood (setMood),
// unless something pushed an override (pushMood), e.g. combat. The highest
// priority push wins (the latest one on ties); popping it returns to whatever
// is underneath, crossfading. A mood that comes back within a few seconds
// resumes its track instead of starting a new one.
//
//   auto& music = audio().music();
//   music.addMood({.name = "Combat", .tracks = {"audio/fight.ogg"}, .fadeIn = 1.0f});
//   music.setMood("Explore");             // the base mood
//   music.pushMood("Combat", 10);         // a fight starts (priority 10)...
//   music.popMood("Combat");              // ...and ends: back to Explore
//   music.stinger("audio/victory.ogg");   // plays over the music, which ducks
//
// In scenes, moods are MusicMood components (the object's name is the mood's
// name) and MusicZone components push a mood while the listener is inside.
// Everything plays on the "Music" bus, which AudioZones leave alone unless
// they list it.
// =============================================================================

#include "ve/scene/GameObject.hpp"

#include <glm/glm.hpp>

#include <map>
#include <random>
#include <string>
#include <vector>

struct ma_sound;

namespace ve {

class AudioSystem;

struct MusicMoodDesc {
    std::string name;
    std::vector<std::string> tracks; // asset paths; none = silence
    bool shuffle = true;
    float volume = 1.0f;
    float fadeIn = 3.0f;             // s to crossfade into this mood
    glm::vec2 gap{8.0f, 25.0f};      // s of silence between tracks (random in range)
};

class MusicPlayer {
public:
    explicit MusicPlayer(AudioSystem& audio) : m_audio(audio) {}
    ~MusicPlayer() { release(); }

    // Adds or replaces a mood (replacing keeps a playing track going).
    void addMood(MusicMoodDesc mood);
    void removeMood(const std::string& name);
    const MusicMoodDesc* mood(const std::string& name) const;

    // fade < 0 = the new mood's fadeIn. "" = silence.
    void setMood(const std::string& name, float fade = -1.0f);
    const std::string& baseMood() const { return m_base; }
    // Overrides the base mood while pushed. Pushes of the same name are
    // counted (two zones + combat...); the mood stays until all are popped.
    void pushMood(const std::string& name, int priority = 0, float fade = -1.0f);
    void popMood(const std::string& name, float fade = -1.0f);
    const std::string& currentMood() const { return m_current; } // what's playing (or should be)
    std::string currentTrack() const;

    // A one-shot over the music; the music ducks to `duck` while it plays.
    void stinger(const std::string& path, float volume = 1.0f, float duck = 0.35f);
    void nextTrack(); // skip to the next track of the current mood
    void setVolume(float v) { m_volume = v; } // on top of mood volumes (the Music bus is another knob)

    // Internal (AudioSystem).
    void update(float dt);
    void release();
    void onGui();

private:
    struct Voice {
        ma_sound* sound = nullptr;
        std::string mood, track;
        float volume = 0.0f; // fade level 0..1
        float target = 1.0f;
        float rate = 1.0f;   // fade units per second
        float idle = 0.0f;   // s spent faded out (paused); freed after a while
    };
    struct Push {
        std::string name;
        int priority = 0;
        int count = 0;
        uint64_t order = 0;
    };
    struct MoodState {
        std::vector<int> order; // shuffled play order
        size_t next = 0;
        int last = -1;
    };

    void select(float fade);       // re-evaluates which mood should play
    void startTrack(float fade);   // starts the next track of m_current
    Voice* currentVoice();
    ma_sound* openTrack(const std::string& path);
    void freeVoice(Voice& v);
    float randomGap(const MusicMoodDesc& m);

    AudioSystem& m_audio;
    std::vector<MusicMoodDesc> m_moods;
    std::map<std::string, MoodState> m_state;
    std::vector<Push> m_pushes;
    std::vector<Voice> m_voices;
    std::vector<ma_sound*> m_stingers;
    std::string m_base, m_current;
    float m_gapTimer = -1.0f; // >= 0: counting down to the next track
    float m_duck = 1.0f, m_duckTarget = 1.0f;
    float m_volume = 1.0f;
    uint64_t m_pushOrder = 0;
    std::mt19937 m_rng{12345};
};

// One music mood in a scene; the object's name is the mood's name.
class MusicMood : public Component {
public:
    std::string tracks;           // "audio/a.ogg; audio/b.ogg"
    bool shuffle = true;
    float volume = 1.0f;
    float fadeIn = 3.0f;
    glm::vec2 gap{8.0f, 25.0f};
    bool playOnStart = false;     // becomes the base mood when the scene starts

    void start() override;
    void update(float dt) override;
    void onDestroy() override;
    void apply(); // (re)registers with the music player (the editor calls this on edits)

private:
    std::string m_registered;
};

// Pushes a mood while the listener is within `radius` (a sphere, or a
// vertical cylinder when height > 0, from the object up).
class MusicZone : public Component {
public:
    std::string mood;
    float radius = 15.0f;
    float height = 0.0f;
    float exitMargin = 2.0f; // leave only once this much further out (no flapping at the edge)
    int priority = 0;

    bool listenerInside() const { return m_inside; }
    void update(float dt) override;
    void onDestroy() override;

private:
    bool m_inside = false;
    std::string m_pushed;
};

} // namespace ve
