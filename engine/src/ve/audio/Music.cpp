#include "ve/audio/Music.hpp"

#include "ve/assets/AssetManager.hpp"
#include "ve/audio/Audio.hpp"
#include "ve/core/Engine.hpp"
#include "ve/core/Log.hpp"
#include "ve/core/Strings.hpp"

#include <imgui.h>
#include <miniaudio.h>

#include <algorithm>

namespace ve {

namespace {

constexpr float kResumeWindow = 20.0f; // s a faded-out track stays paused (to resume) before it's freed

} // namespace

// -----------------------------------------------------------------------------
// MusicPlayer
// -----------------------------------------------------------------------------

void MusicPlayer::addMood(MusicMoodDesc mood)
{
    const std::string name = mood.name;
    auto it = std::find_if(m_moods.begin(), m_moods.end(), [&](const MusicMoodDesc& m) { return m.name == name; });
    if (it != m_moods.end()) {
        if (it->tracks != mood.tracks) m_state.erase(name); // new playlist: new order
        *it = std::move(mood);
    } else {
        m_moods.push_back(std::move(mood));
    }
    // The mood was asked for before it existed (or got new tracks while silent).
    if (name == m_current && !currentVoice() && m_gapTimer < 0.0f) startTrack(this->mood(name)->fadeIn);
}

void MusicPlayer::removeMood(const std::string& name)
{
    // A playing track carries on to its end; the mood is just no longer known.
    std::erase_if(m_moods, [&](const MusicMoodDesc& m) { return m.name == name; });
    m_state.erase(name);
}

const MusicMoodDesc* MusicPlayer::mood(const std::string& name) const
{
    for (const MusicMoodDesc& m : m_moods) {
        if (m.name == name) return &m;
    }
    return nullptr;
}

void MusicPlayer::setMood(const std::string& name, float fade)
{
    m_base = name;
    select(fade);
}

void MusicPlayer::pushMood(const std::string& name, int priority, float fade)
{
    auto it = std::find_if(m_pushes.begin(), m_pushes.end(), [&](const Push& p) { return p.name == name; });
    if (it == m_pushes.end()) {
        m_pushes.push_back({name, priority, 1, ++m_pushOrder});
    } else {
        ++it->count;
        it->priority = std::max(it->priority, priority);
    }
    select(fade);
}

void MusicPlayer::popMood(const std::string& name, float fade)
{
    auto it = std::find_if(m_pushes.begin(), m_pushes.end(), [&](const Push& p) { return p.name == name; });
    if (it == m_pushes.end()) return;
    if (--it->count <= 0) m_pushes.erase(it);
    select(fade);
}

std::string MusicPlayer::currentTrack() const
{
    for (const Voice& v : m_voices) {
        if (v.mood == m_current && v.target > 0.0f) return v.track;
    }
    return {};
}

MusicPlayer::Voice* MusicPlayer::currentVoice()
{
    for (Voice& v : m_voices) {
        if (v.mood == m_current && v.target > 0.0f) return &v;
    }
    return nullptr;
}

void MusicPlayer::select(float fade)
{
    std::string want = m_base;
    const Push* top = nullptr;
    for (const Push& p : m_pushes) {
        if (!top || p.priority > top->priority || (p.priority == top->priority && p.order > top->order)) top = &p;
    }
    if (top) want = top->name;
    if (want == m_current) return;

    m_current = want;
    const MusicMoodDesc* m = mood(want);
    const float f = fade >= 0.0f ? fade : (m ? m->fadeIn : 2.0f);
    const float rate = 1.0f / std::max(f, 0.01f);
    m_gapTimer = -1.0f;

    Voice* resume = nullptr;
    for (Voice& v : m_voices) {
        v.rate = rate;
        if (v.mood == want) {
            resume = &v; // the newest one wins (voices are in start order)
        } else {
            v.target = 0.0f;
        }
    }
    if (resume) {
        for (Voice& v : m_voices) {
            if (&v != resume && v.mood == want) v.target = 0.0f;
        }
        resume->target = 1.0f;
        resume->idle = 0.0f;
        if (!ma_sound_is_playing(resume->sound.get())) ma_sound_start(resume->sound.get());
        return;
    }
    startTrack(f);
}

SoundPtr MusicPlayer::openTrack(const std::string& path)
{
    if (!m_audio.available()) return nullptr;
    std::string file;
    try {
        // (loadAudio: the cooked Ogg when current, also in a packaged game.)
        file = Engine::get().assets().loadAudio(path, true)->path();
    } catch (const std::exception&) {
        log::warnOnce("Music: track not found: " + path);
        return nullptr;
    }
    SoundPtr sound = openSound(m_audio.engine(), file, MA_SOUND_FLAG_STREAM, m_audio.bus("Music").group.get());
    if (!sound) {
        log::warnOnce("Music: failed to load " + path);
        return nullptr;
    }
    ma_sound_set_spatialization_enabled(sound.get(), MA_FALSE);
    return sound;
}

void MusicPlayer::startTrack(float fade)
{
    const MusicMoodDesc* m = mood(m_current);
    if (!m || m->tracks.empty()) return;
    MoodState& st = m_state[m->name];
    const int n = static_cast<int>(m->tracks.size());
    int index = 0;
    if (m->shuffle) {
        // Shuffled order, reshuffled each round, never the same track twice in a row.
        if (st.next >= st.order.size() || static_cast<int>(st.order.size()) != n) {
            st.order.resize(n);
            for (int i = 0; i < n; ++i) st.order[i] = i;
            std::shuffle(st.order.begin(), st.order.end(), m_rng);
            if (n > 1 && st.order.front() == st.last) std::swap(st.order.front(), st.order.back());
            st.next = 0;
        }
        index = st.order[st.next++];
    } else {
        index = (st.last + 1) % n;
    }
    st.last = index;

    SoundPtr sound = openTrack(m->tracks[index]);
    if (!sound) {
        m_gapTimer = randomGap(*m); // try the next one later
        return;
    }
    ma_sound_set_volume(sound.get(), 0.0f);
    ma_sound_start(sound.get());
    Voice v;
    v.sound = std::move(sound);
    v.mood = m->name;
    v.track = m->tracks[index];
    v.volume = fade > 0.0f ? 0.0f : 1.0f;
    v.target = 1.0f;
    v.rate = 1.0f / std::max(fade, 0.01f);
    m_voices.push_back(std::move(v));
}

float MusicPlayer::randomGap(const MusicMoodDesc& m)
{
    const float lo = std::max(0.0f, std::min(m.gap.x, m.gap.y));
    const float hi = std::max(lo, std::max(m.gap.x, m.gap.y));
    return std::uniform_real_distribution<float>(lo, hi)(m_rng);
}

void MusicPlayer::nextTrack()
{
    if (Voice* v = currentVoice()) {
        v->target = 0.0f;
        v->rate = 1.0f;
        v->mood.clear(); // don't resume it
    }
    m_gapTimer = -1.0f;
    startTrack(1.0f);
}

void MusicPlayer::stinger(const std::string& path, float volume, float duck)
{
    if (!m_audio.available()) return;
    std::string file;
    try {
        file = Engine::get().assets().loadAudio(path)->path();
    } catch (const std::exception&) {
        log::warnOnce("Music: stinger not found: " + path);
        return;
    }
    SoundPtr sound = openSound(m_audio.engine(), file, MA_SOUND_FLAG_DECODE, m_audio.bus("Music").group.get());
    if (!sound) {
        log::warnOnce("Music: failed to load " + path);
        return;
    }
    ma_sound_set_spatialization_enabled(sound.get(), MA_FALSE);
    ma_sound_set_volume(sound.get(), volume * m_volume);
    ma_sound_start(sound.get());
    m_stingers.push_back(std::move(sound));
    m_duckTarget = std::min(m_duckTarget, duck);
}

void MusicPlayer::freeVoice(Voice& v)
{
    v.sound.reset();
}

void MusicPlayer::update(float dt)
{
    // Stingers: free finished ones; the music comes back up once they're done.
    std::erase_if(m_stingers, [](const SoundPtr& s) { return ma_sound_at_end(s.get()); });
    if (m_stingers.empty()) m_duckTarget = 1.0f;
    m_duck += (m_duckTarget - m_duck) * (1.0f - std::exp(-dt * (m_duckTarget < m_duck ? 12.0f : 1.5f)));

    bool trackEnded = false;
    for (Voice& v : m_voices) {
        const float step = v.rate * dt;
        v.volume = v.target > v.volume ? std::min(v.target, v.volume + step) : std::max(v.target, v.volume - step);
        const MusicMoodDesc* m = mood(v.mood);
        // Equal-power-ish curve: crossfades don't dip in the middle.
        const float curve = std::sin(v.volume * 1.5707963f);
        ma_sound_set_volume(v.sound.get(), curve * (m ? m->volume : 1.0f) * m_duck * m_volume);
        if (v.target <= 0.0f && v.volume <= 0.0f) {
            if (ma_sound_is_playing(v.sound.get())) ma_sound_stop(v.sound.get()); // paused: can resume
            v.idle += dt;
            if (v.idle > kResumeWindow || v.mood.empty()) freeVoice(v);
        } else if (ma_sound_at_end(v.sound.get())) {
            if (v.mood == m_current && v.target > 0.0f) trackEnded = true;
            freeVoice(v);
        }
    }
    std::erase_if(m_voices, [](const Voice& v) { return v.sound == nullptr; });

    if (trackEnded) {
        if (const MusicMoodDesc* m = mood(m_current)) m_gapTimer = randomGap(*m);
    }
    if (m_gapTimer >= 0.0f) {
        m_gapTimer -= dt;
        if (m_gapTimer < 0.0f) startTrack(0.5f);
    }
}

void MusicPlayer::release()
{
    for (Voice& v : m_voices) freeVoice(v);
    m_voices.clear();
    m_stingers.clear();
}

void MusicPlayer::onGui()
{
    if (!ImGui::TreeNode("Music")) return;
    ImGui::Text("Mood: %s (base %s)", m_current.empty() ? "-" : m_current.c_str(), m_base.empty() ? "-" : m_base.c_str());
    for (const Push& p : m_pushes) ImGui::BulletText("pushed %s (priority %d, x%d)", p.name.c_str(), p.priority, p.count);
    const std::string track = currentTrack();
    if (!track.empty()) ImGui::Text("Track: %s", track.c_str());
    else if (m_gapTimer >= 0.0f) ImGui::Text("Next track in %.0f s", m_gapTimer);
    for (const MusicMoodDesc& m : m_moods) {
        if (ImGui::SmallButton(m.name.c_str())) setMood(m.name);
        ImGui::SameLine();
    }
    if (ImGui::SmallButton("Next track")) nextTrack();
    ImGui::TreePop();
}

// -----------------------------------------------------------------------------
// MusicMood / MusicZone
// -----------------------------------------------------------------------------

void MusicMood::apply()
{
    MusicPlayer& music = audio().music();
    const std::string name = gameObject().name();
    if (!m_registered.empty() && m_registered != name) music.removeMood(m_registered);
    MusicMoodDesc d;
    d.name = name;
    d.tracks = str::split(tracks, ';');
    d.shuffle = shuffle;
    d.volume = volume;
    d.fadeIn = fadeIn;
    d.gap = gap;
    music.addMood(std::move(d));
    m_registered = name;
}

void MusicMood::start()
{
    apply();
    if (playOnStart) audio().music().setMood(m_registered);
}

void MusicMood::update(float)
{
    if (gameObject().name() != m_registered) apply(); // renamed in the editor
}

void MusicMood::onDestroy()
{
    if (!m_registered.empty()) audio().music().removeMood(m_registered);
    m_registered.clear();
}

void MusicZone::update(float)
{
    const glm::vec3 local = audio().listenerPosition() - transform().position();
    const float margin = m_inside ? exitMargin : 0.0f;
    bool inside = glm::length(height > 0.0f ? glm::vec2(local.x, local.z) : glm::vec2(glm::length(local), 0.0f)) <=
                  radius + margin;
    if (height > 0.0f) inside = inside && local.y >= -margin && local.y <= height + margin;
    inside = inside && !mood.empty();

    MusicPlayer& music = audio().music();
    if (inside && m_pushed != mood) {
        if (!m_pushed.empty()) music.popMood(m_pushed);
        music.pushMood(mood, priority);
        m_pushed = mood;
    } else if (!inside && !m_pushed.empty()) {
        music.popMood(m_pushed);
        m_pushed.clear();
    }
    m_inside = inside;
}

void MusicZone::onDestroy()
{
    if (!m_pushed.empty()) audio().music().popMood(m_pushed);
    m_pushed.clear();
    m_inside = false;
}

} // namespace ve
