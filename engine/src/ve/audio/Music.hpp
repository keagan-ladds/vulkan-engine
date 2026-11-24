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

#include "ve/audio/Audio.hpp" // SoundPtr
#include "ve/scene/GameObject.hpp"

#include <glm/glm.hpp>

#include <map>
#include <random>
#include <string>
#include <vector>

struct ma_sound;

namespace ve {

class AudioSystem;

/// A music mood: a playlist with its own volume, crossfade time and gaps.
struct MusicMoodDesc {
    std::string name;                ///< Identifies the mood (setMood / pushMood use it).
    std::vector<std::string> tracks; ///< Asset paths of the tracks; empty = the mood is silence.
    bool shuffle = true;             ///< Play the tracks in a shuffled order (no repeat until all played).
    float volume = 1.0f;             ///< Linear volume of this mood's tracks (0..1, before the player's volume).
    float fadeIn = 3.0f;             ///< Seconds to crossfade into this mood when it becomes the playing one.
    glm::vec2 gap{8.0f, 25.0f};      ///< Seconds of silence between tracks: random in [x, y].
};

/// Plays music by mood: one mood at a time, crossfaded, with a stack of
/// overrides and one-shot stingers. Owned by the AudioSystem (`audio().music()`).
/// Main thread only.
class MusicPlayer {
public:
    /// Created by the AudioSystem; games use `audio().music()`.
    explicit MusicPlayer(AudioSystem& audio) : m_audio(audio) {}
    ~MusicPlayer() { release(); }

    /// Adds a mood, or replaces the one with the same name (a track of it that
    /// is playing keeps playing).
    void addMood(MusicMoodDesc mood);
    /// Removes a mood; if it was playing, the music fades to whatever should play instead.
    void removeMood(const std::string& name);
    /// The mood called `name`, or null.
    const MusicMoodDesc* mood(const std::string& name) const;

    /// Sets the base mood: what plays when no pushed mood overrides it.
    /// @param name the mood's name; "" = silence.
    /// @param fade crossfade time in seconds; < 0 = the new mood's own fadeIn.
    void setMood(const std::string& name, float fade = -1.0f);
    /// The base mood set by setMood ("" = silence).
    const std::string& baseMood() const { return m_base; }
    /// Overrides the base mood until popped (combat, a zone, a cutscene).
    /// Pushes of the same name are counted (two zones and a fight can push the
    /// same mood); it stays until each push is popped.
    /// @param name the mood to play.
    /// @param priority the highest-priority push plays; ties go to the latest push.
    /// @param fade crossfade time in seconds; < 0 = the mood's own fadeIn.
    void pushMood(const std::string& name, int priority = 0, float fade = -1.0f);
    /// Undoes one pushMood(name); when its count reaches 0 the music returns
    /// to the next push down, or the base mood.
    /// @param fade crossfade time in seconds; < 0 = the mood that takes over's fadeIn.
    void popMood(const std::string& name, float fade = -1.0f);
    /// The mood that is playing (or about to, during a gap); "" = silence.
    const std::string& currentMood() const { return m_current; }
    /// Asset path of the track playing now; "" between tracks or in silence.
    std::string currentTrack() const;

    /// Plays a one-shot sound over the music (a victory jingle, a discovery),
    /// lowering the music while it plays and bringing it back afterwards.
    /// @param path the sound's asset path.
    /// @param volume the stinger's own volume (0..1).
    /// @param duck the music's volume while the stinger plays, as a fraction of
    ///        its normal volume (0 = silent, 1 = not lowered).
    void stinger(const std::string& path, float volume = 1.0f, float duck = 0.35f);
    /// Skips to the next track of the current mood (after its usual gap).
    void nextTrack();
    /// Overall music volume (0..1), multiplied with each mood's volume. The
    /// "Music" bus volume is a separate setting on top (audio().setBusVolume).
    void setVolume(float v) { m_volume = v; }

    /// Advances fades, gaps and track changes. Called by the AudioSystem each frame.
    void update(float dt);
    /// Stops and frees every voice and stinger (shutdown).
    void release();
    /// Debug panel contents (moods, pushes, voices) for the audio debug window.
    void onGui();

private:
    struct Voice {
        SoundPtr sound;
        std::string mood, track;
        float volume = 0.0f; ///< fade level 0..1
        float target = 1.0f;
        float rate = 1.0f;   ///< fade units per second
        float idle = 0.0f;   ///< s spent faded out (paused); freed after a while
    };
    struct Push {
        std::string name;
        int priority = 0;
        int count = 0;
        uint64_t order = 0;
    };
    struct MoodState {
        std::vector<int> order; ///< shuffled play order
        size_t next = 0;
        int last = -1;
    };

    void select(float fade);       ///< re-evaluates which mood should play
    void startTrack(float fade);   ///< starts the next track of m_current
    Voice* currentVoice();
    SoundPtr openTrack(const std::string& path);
    void freeVoice(Voice& v);
    float randomGap(const MusicMoodDesc& m);

    AudioSystem& m_audio;
    std::vector<MusicMoodDesc> m_moods;
    std::map<std::string, MoodState> m_state;
    std::vector<Push> m_pushes;
    std::vector<Voice> m_voices;
    std::vector<SoundPtr> m_stingers;
    std::string m_base, m_current;
    float m_gapTimer = -1.0f; ///< >= 0: counting down to the next track
    float m_duck = 1.0f, m_duckTarget = 1.0f;
    float m_volume = 1.0f;
    uint64_t m_pushOrder = 0;
    std::mt19937 m_rng{12345};
};

/// A music mood placed in a scene (registered with the MusicPlayer while the
/// component exists). The object's name is the mood's name.
class MusicMood : public Component {
public:
    std::string tracks;           ///< Track asset paths separated by ';': "audio/a.ogg; audio/b.ogg".
    bool shuffle = true;          ///< See MusicMoodDesc::shuffle.
    float volume = 1.0f;          ///< See MusicMoodDesc::volume.
    float fadeIn = 3.0f;          ///< See MusicMoodDesc::fadeIn (seconds).
    glm::vec2 gap{8.0f, 25.0f};   ///< See MusicMoodDesc::gap (seconds, random in [x, y]).
    bool playOnStart = false;     ///< Becomes the base mood when the scene starts.

    void start() override;
    void update(float dt) override;
    void onDestroy() override;
    /// (Re)registers the mood with the MusicPlayer from the fields; the editor
    /// calls it after edits, code after changing fields at runtime.
    void apply();

private:
    std::string m_registered;
};

/// Pushes a mood while the listener is within `radius` (a sphere, or a
/// vertical cylinder when height > 0, from the object up).
class MusicZone : public Component {
public:
    std::string mood;        ///< Name of the mood to push (a MusicMood or MusicPlayer::addMood).
    float radius = 15.0f;    ///< Metres from the object's position.
    float height = 0.0f;     ///< > 0: a vertical cylinder this tall (from the object up) instead of a sphere.
    float exitMargin = 2.0f; ///< Metres further out the listener must go to leave (no flapping at the edge).
    int priority = 0;        ///< Push priority (MusicPlayer::pushMood).

    /// Whether the listener (the camera's audio listener) is in the zone, i.e. the mood is pushed.
    bool listenerInside() const { return m_inside; }
    void update(float dt) override;
    void onDestroy() override;

private:
    bool m_inside = false;
    std::string m_pushed;
};

} // namespace ve
