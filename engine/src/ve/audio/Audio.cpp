#include "ve/audio/Audio.hpp"

#include "ve/core/Engine.hpp"
#include "ve/core/Log.hpp"

#include <miniaudio.h>

namespace ve {

// -----------------------------------------------------------------------------
// AudioSystem
// -----------------------------------------------------------------------------

AudioSystem::~AudioSystem()
{
    shutdown();
}

void AudioSystem::init()
{
    m_engine = new ma_engine();
    ma_engine_config config = ma_engine_config_init();
    if (ma_engine_init(&config, m_engine) != MA_SUCCESS) {
        log::warn("Audio: no output device available; sound disabled");
        delete m_engine;
        m_engine = nullptr;
        return;
    }
    log::info("Audio: {} Hz, {} channels", ma_engine_get_sample_rate(m_engine), ma_engine_get_channels(m_engine));
}

void AudioSystem::shutdown()
{
    if (!m_engine) return;
    for (ma_sound* s : m_oneShots) {
        ma_sound_uninit(s);
        delete s;
    }
    m_oneShots.clear();
    ma_engine_uninit(m_engine);
    delete m_engine;
    m_engine = nullptr;
}

void AudioSystem::setListener(const glm::vec3& p, const glm::vec3& f, const glm::vec3& u)
{
    if (!m_engine) return;
    ma_engine_listener_set_position(m_engine, 0, p.x, p.y, p.z);
    ma_engine_listener_set_direction(m_engine, 0, f.x, f.y, f.z);
    ma_engine_listener_set_world_up(m_engine, 0, u.x, u.y, u.z);
}

void AudioSystem::setMasterVolume(float volume)
{
    m_masterVolume = volume;
    if (m_engine) ma_engine_set_volume(m_engine, volume);
}

void AudioSystem::playOneShot(const AudioClip& clip, float volume, std::optional<glm::vec3> position, float pitch)
{
    if (!m_engine) return;
    auto* sound = new ma_sound();
    const ma_uint32 flags = MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_ASYNC;
    if (ma_sound_init_from_file(m_engine, clip.path().c_str(), flags, nullptr, nullptr, sound) != MA_SUCCESS) {
        log::warn("Audio: failed to load {}", clip.path());
        delete sound;
        return;
    }
    ma_sound_set_volume(sound, volume);
    ma_sound_set_pitch(sound, pitch);
    if (position) {
        ma_sound_set_position(sound, position->x, position->y, position->z);
    } else {
        ma_sound_set_spatialization_enabled(sound, MA_FALSE);
    }
    ma_sound_start(sound);
    m_oneShots.push_back(sound);
}

void AudioSystem::update()
{
    if (!m_engine) return;
    std::erase_if(m_oneShots, [](ma_sound* s) {
        if (ma_sound_at_end(s)) {
            ma_sound_uninit(s);
            delete s;
            return true;
        }
        return false;
    });
}

// -----------------------------------------------------------------------------
// AudioSource
// -----------------------------------------------------------------------------

AudioSource::~AudioSource()
{
    release();
}

void AudioSource::release()
{
    if (m_loaded) {
        ma_sound_uninit(m_sound);
        m_loaded = false;
    }
    delete m_sound;
    m_sound = nullptr;
}

void AudioSource::start()
{
    if (playOnStart) play();
}

void AudioSource::play()
{
    AudioSystem& sys = audio();
    if (!sys.available() || !clip) return;
    if (!m_loaded) {
        m_sound = new ma_sound();
        ma_uint32 flags = clip->stream() ? MA_SOUND_FLAG_STREAM : MA_SOUND_FLAG_DECODE;
        if (ma_sound_init_from_file(sys.engine(), clip->path().c_str(), flags, nullptr, nullptr, m_sound) !=
            MA_SUCCESS) {
            log::warn("Audio: failed to load {}", clip->path());
            delete m_sound;
            m_sound = nullptr;
            return;
        }
        m_loaded = true;
    }
    applySettings();
    const glm::vec3 p = transform().position();
    ma_sound_set_position(m_sound, p.x, p.y, p.z);
    ma_sound_seek_to_pcm_frame(m_sound, 0);
    ma_sound_start(m_sound);
}

void AudioSource::applySettings()
{
    if (!m_loaded) return;
    ma_sound_set_volume(m_sound, volume);
    ma_sound_set_pitch(m_sound, pitch);
    ma_sound_set_looping(m_sound, loop ? MA_TRUE : MA_FALSE);
    ma_sound_set_spatialization_enabled(m_sound, spatial ? MA_TRUE : MA_FALSE);
    ma_sound_set_min_distance(m_sound, minDistance);
    ma_sound_set_max_distance(m_sound, maxDistance);
    ma_sound_set_attenuation_model(m_sound, ma_attenuation_model_linear);
}

void AudioSource::stop()
{
    if (m_loaded) ma_sound_stop(m_sound);
}

bool AudioSource::isPlaying() const
{
    return m_loaded && ma_sound_is_playing(m_sound);
}

void AudioSource::lateUpdate(float)
{
    if (!m_loaded || !spatial) return;
    const glm::vec3 p = transform().position();
    ma_sound_set_position(m_sound, p.x, p.y, p.z);
}

void AudioSource::onDestroy()
{
    release();
}

} // namespace ve
