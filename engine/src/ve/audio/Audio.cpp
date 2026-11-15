#include "ve/audio/Audio.hpp"

#include "ve/core/Engine.hpp"
#include "ve/core/Log.hpp"
#include "ve/physics/Physics.hpp"

#include <glm/gtc/quaternion.hpp>
#include <imgui.h>
#include <miniaudio.h>

#include <algorithm>
#include <cmath>

namespace ve {

// A 2nd-order low-pass node in miniaudio's node graph.
struct AudioFilter {
    ma_lpf_node node{};
    float cutoff = AudioSystem::kNoFilter;
};

namespace {

constexpr ma_uint32 kFilterOrder = 2;

AudioFilter* createFilter(ma_engine* engine)
{
    auto* f = new AudioFilter();
    const ma_uint32 channels = ma_engine_get_channels(engine);
    const ma_uint32 rate = ma_engine_get_sample_rate(engine);
    const ma_lpf_node_config cfg = ma_lpf_node_config_init(channels, rate, std::min(AudioSystem::kNoFilter, rate * 0.45f), kFilterOrder);
    if (ma_lpf_node_init(ma_engine_get_node_graph(engine), &cfg, nullptr, &f->node) != MA_SUCCESS) {
        delete f;
        return nullptr;
    }
    return f;
}

void destroyFilter(AudioFilter* f)
{
    if (!f) return;
    ma_lpf_node_uninit(&f->node, nullptr);
    delete f;
}

// Changes the cutoff (skipped for tiny changes; reinit keeps the filter state).
void setCutoff(ma_engine* engine, AudioFilter* f, float hz)
{
    if (!f) return;
    const float nyquistSafe = ma_engine_get_sample_rate(engine) * 0.45f;
    hz = std::clamp(hz, 40.0f, std::min(AudioSystem::kNoFilter, nyquistSafe));
    if (std::abs(hz - f->cutoff) < f->cutoff * 0.01f) return;
    f->cutoff = hz;
    const ma_lpf_config cfg = ma_lpf_config_init(ma_format_f32, ma_engine_get_channels(engine), ma_engine_get_sample_rate(engine), hz, kFilterOrder);
    ma_lpf_node_reinit(&cfg, &f->node);
}

// Low-pass interpolation in log-frequency (perceptually even).
float mixCutoff(float a, float b, float t)
{
    return std::exp(std::log(a) + (std::log(b) - std::log(a)) * t);
}

} // namespace

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
    for (const char* name : {"SFX", "Ambient", "Weather", "Music", "UI"}) bus(name);
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
    for (auto& b : m_buses) {
        if (b->group) {
            ma_sound_group_uninit(b->group);
            delete b->group;
        }
        destroyFilter(b->filter);
    }
    m_buses.clear();
    ma_engine_uninit(m_engine);
    delete m_engine;
    m_engine = nullptr;
}

AudioBus& AudioSystem::bus(const std::string& name)
{
    for (auto& b : m_buses) {
        if (b->name == name) return *b;
    }
    auto b = std::make_unique<AudioBus>();
    b->name = name;
    if (m_engine) {
        // sounds -> group -> low-pass -> output
        b->group = new ma_sound();
        if (ma_sound_group_init(m_engine, 0, nullptr, b->group) != MA_SUCCESS) {
            delete b->group;
            b->group = nullptr;
        }
        b->filter = createFilter(m_engine);
        if (b->group && b->filter) {
            ma_node_attach_output_bus(b->group, 0, &b->filter->node, 0);
            ma_node_attach_output_bus(&b->filter->node, 0, ma_engine_get_endpoint(m_engine), 0);
        }
    }
    m_buses.push_back(std::move(b));
    return *m_buses.back();
}

void AudioSystem::setListener(const glm::vec3& p, const glm::vec3& f, const glm::vec3& u)
{
    m_listener = p;
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

void AudioSystem::playOneShot(const AudioClip& clip, float volume, std::optional<glm::vec3> position, float pitch,
                              const std::string& busName)
{
    if (!m_engine) return;
    auto* sound = new ma_sound();
    const ma_uint32 flags = MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_ASYNC;
    if (ma_sound_init_from_file(m_engine, clip.path().c_str(), flags, bus(busName).group, nullptr, sound) != MA_SUCCESS) {
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

void AudioSystem::update(float dt)
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

    // Zones: which ones hold the listener, and the strongest effect per bus.
    for (AudioZone* z : m_zones) z->m_inside = z->isActive() && z->contains(m_listener);
    for (auto& b : m_buses) {
        float targetVolume = 1.0f, targetLowpass = kNoFilter, fade = 0.35f;
        for (AudioZone* z : m_zones) {
            if (!z->m_inside) continue;
            for (const AudioZone::Effect& e : z->effects) {
                if (e.bus != b->name) continue;
                targetVolume = std::min(targetVolume, e.volume);
                targetLowpass = std::min(targetLowpass, e.lowpass);
                fade = z->fadeTime;
            }
        }
        // Exponential approach: ~95 % of the way after fadeTime.
        const float k = 1.0f - std::exp(-3.0f * dt / std::max(fade, 0.01f));
        b->zoneVolume += (targetVolume - b->zoneVolume) * k;
        b->zoneLowpass = mixCutoff(b->zoneLowpass, targetLowpass, k);
        if (b->group) ma_sound_group_set_volume(b->group, b->volume * b->zoneVolume);
        setCutoff(m_engine, b->filter, std::min(b->lowpass, b->zoneLowpass));
    }
}

void AudioSystem::onGui()
{
    for (auto& b : m_buses) {
        ImGui::PushID(b.get());
        ImGui::SliderFloat(b->name.c_str(), &b->volume, 0.0f, 1.5f);
        ImGui::SameLine();
        ImGui::TextDisabled("x%.2f, %.0f Hz", b->zoneVolume, b->filter ? b->filter->cutoff : 0.0f);
        ImGui::PopID();
    }
    int inside = 0;
    for (AudioZone* z : m_zones) inside += z->m_inside ? 1 : 0;
    ImGui::Text("Zones: %zu (listener inside %d)", m_zones.size(), inside);
}

// -----------------------------------------------------------------------------
// AudioZone
// -----------------------------------------------------------------------------

bool AudioZone::contains(const glm::vec3& worldPoint) const
{
    const Transform& t = transform();
    glm::vec3 scale = glm::vec3(glm::length(glm::vec3(t.worldMatrix()[0])), glm::length(glm::vec3(t.worldMatrix()[1])),
                                glm::length(glm::vec3(t.worldMatrix()[2])));
    scale = glm::max(scale, glm::vec3(1e-6f));
    const glm::vec3 local = glm::inverse(t.rotation()) * (worldPoint - t.position()) / scale;
    switch (shape) {
    case Shape::Box: return glm::all(glm::lessThanEqual(glm::abs(local), halfExtents));
    case Shape::Sphere: return glm::length(local) <= radius;
    case Shape::Cylinder: return local.y >= 0.0f && local.y <= height && glm::length(glm::vec2(local.x, local.z)) <= radius;
    }
    return false;
}

void AudioZone::start()
{
    audio().addZone(this);
    m_registered = true;
}

void AudioZone::unregister()
{
    if (m_registered) audio().removeZone(this);
    m_registered = false;
    m_inside = false;
}

void AudioZone::onDestroy() { unregister(); }

AudioZone::~AudioZone() { unregister(); }

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
    destroyFilter(m_filter);
    m_filter = nullptr;
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
        AudioBus& b = sys.bus(bus);
        const bool filtered = occlusion && spatial;
        m_sound = new ma_sound();
        ma_uint32 flags = clip->stream() ? MA_SOUND_FLAG_STREAM : MA_SOUND_FLAG_DECODE;
        // Occluded sources get their own low-pass: sound -> filter -> bus.
        if (ma_sound_init_from_file(sys.engine(), clip->path().c_str(), flags, filtered ? nullptr : b.group, nullptr,
                                    m_sound) != MA_SUCCESS) {
            log::warn("Audio: failed to load {}", clip->path());
            delete m_sound;
            m_sound = nullptr;
            return;
        }
        m_loaded = true;
        if (filtered && (m_filter = createFilter(sys.engine()))) {
            ma_node_attach_output_bus(m_sound, 0, &m_filter->node, 0);
            ma_node_attach_output_bus(&m_filter->node, 0, b.group ? static_cast<ma_node*>(b.group) : ma_engine_get_endpoint(sys.engine()), 0);
        }
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
    ma_sound_set_volume(m_sound, volume * (1.0f + (occlusionVolume - 1.0f) * m_occluded));
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

void AudioSource::updateOcclusion(float dt)
{
    const glm::vec3 listener = audio().listenerPosition();
    const glm::vec3 source = transform().position();
    const float dist = glm::length(source - listener);

    // A few rays per second (staggered per source), to the source and two
    // points beside it: partial blocking (a doorway, a thin post) gives
    // partial muffling.
    m_rayTimer -= dt;
    if (m_rayTimer <= 0.0f) {
        m_rayTimer = 0.12f + 0.04f * static_cast<float>(reinterpret_cast<uintptr_t>(this) % 7) / 7.0f;
        if (dist > maxDistance || dist < 0.5f) {
            m_occlusionTarget = 0.0f;
        } else {
            const glm::vec3 dir = (source - listener) / dist;
            glm::vec3 side = glm::cross(dir, glm::vec3(0, 1, 0));
            side = glm::length(side) > 1e-3f ? glm::normalize(side) * 0.5f : glm::vec3(0.5f, 0, 0);
            const Rigidbody* own = gameObject().getComponent<Rigidbody>();
            int blocked = 0;
            for (const glm::vec3& target : {source, source + side, source - side + glm::vec3(0, 0.4f, 0)}) {
                const glm::vec3 d = target - listener;
                const float len = glm::length(d);
                // Stop short of the source so colliders right at it don't count.
                if (physics().raycast(listener, d / len, std::max(len - 0.6f, 0.0f), own)) ++blocked;
            }
            m_occlusionTarget = static_cast<float>(blocked) / 3.0f;
        }
    }
    m_occluded += (m_occlusionTarget - m_occluded) * (1.0f - std::exp(-dt * 8.0f));

    ma_sound_set_volume(m_sound, volume * (1.0f + (occlusionVolume - 1.0f) * m_occluded));
    setCutoff(audio().engine(), m_filter, mixCutoff(AudioSystem::kNoFilter, occlusionLowpass, m_occluded));
}

void AudioSource::lateUpdate(float dt)
{
    if (!m_loaded || !spatial) return;
    const glm::vec3 p = transform().position();
    ma_sound_set_position(m_sound, p.x, p.y, p.z);
    if (occlusion && m_filter) updateOcclusion(dt);
}

void AudioSource::onDestroy()
{
    release();
}

} // namespace ve
