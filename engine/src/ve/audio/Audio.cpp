#include "ve/audio/Audio.hpp"
#include "ve/audio/Music.hpp"

#include "ve/core/Engine.hpp"
#include "ve/core/FileSystem.hpp"
#include "ve/core/Log.hpp"
#include "ve/physics/Physics.hpp"

#include <glm/gtc/quaternion.hpp>
#include <imgui.h>
#include <miniaudio.h>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace ve {

// A 2nd-order low-pass node in miniaudio's node graph: a custom node rather
// than ma_lpf_node, so that cutoff changes happen on the audio thread. The
// main thread only stores the wanted cutoff (an atomic); onProcess picks it up
// before filtering and reinits the filter there (reinit with the same order
// keeps the filter state and allocates nothing). Reiniting from the main
// thread instead would race the audio thread processing the same node.
struct AudioFilter {
    ma_node_base node{}; // must stay first: miniaudio hands us the ma_node* of this struct
    ma_lpf lpf{};
    ma_uint32 channels = 0;
    ma_uint32 sampleRate = 0;
    std::atomic<float> target{AudioSystem::kNoFilter}; // written by the main thread
    float applied = AudioSystem::kNoFilter;             // audio thread only
    float cutoff = AudioSystem::kNoFilter;              // main thread's view (last value sent)
};

void SoundDeleter::operator()(ma_sound* s) const
{
    ma_sound_uninit(s); // (= ma_sound_group_uninit for groups)
    delete s;
}

void FilterDeleter::operator()(AudioFilter* f) const
{
    ma_node_uninit(&f->node, nullptr); // detaches it from the graph first
    ma_lpf_uninit(&f->lpf, nullptr);
    delete f;
}

void EngineDeleter::operator()(ma_engine* e) const
{
    ma_engine_uninit(e);
    delete e;
}

SoundPtr openSound(ma_engine* engine, const std::string& file, uint32_t flags, ma_sound* group)
{
    auto sound = std::make_unique<ma_sound>();
    if (ma_sound_init_from_file(engine, file.c_str(), flags, group, nullptr, sound.get()) != MA_SUCCESS) return nullptr;
    return SoundPtr(sound.release()); // initialized: from now on the deleter uninits it
}

namespace {

constexpr ma_uint32 kFilterOrder = 2;

// Audio thread: apply the latest cutoff, then filter.
void processFilter(ma_node* node, const float** in, ma_uint32* frameCountIn, float** out, ma_uint32* frameCountOut)
{
    (void)frameCountIn;
    auto* f = reinterpret_cast<AudioFilter*>(node);
    const float target = f->target.load(std::memory_order_relaxed);
    if (target != f->applied) {
        f->applied = target;
        const ma_lpf_config cfg = ma_lpf_config_init(ma_format_f32, f->channels, f->sampleRate, target, kFilterOrder);
        ma_lpf_reinit(&cfg, &f->lpf);
    }
    ma_lpf_process_pcm_frames(&f->lpf, out[0], in[0], *frameCountOut);
}

ma_node_vtable g_filterVtable = {processFilter, nullptr, 1, 1, 0}; // one input, one output

FilterPtr createFilter(ma_engine* engine)
{
    auto f = std::make_unique<AudioFilter>();
    f->channels = ma_engine_get_channels(engine);
    f->sampleRate = ma_engine_get_sample_rate(engine);
    const float open = std::min(AudioSystem::kNoFilter, f->sampleRate * 0.45f);
    f->target = f->applied = f->cutoff = open;
    const ma_lpf_config lpf = ma_lpf_config_init(ma_format_f32, f->channels, f->sampleRate, open, kFilterOrder);
    if (ma_lpf_init(&lpf, nullptr, &f->lpf) != MA_SUCCESS) return nullptr;
    ma_node_config cfg = ma_node_config_init();
    cfg.vtable = &g_filterVtable;
    cfg.pInputChannels = &f->channels;
    cfg.pOutputChannels = &f->channels;
    if (ma_node_init(ma_engine_get_node_graph(engine), &cfg, nullptr, &f->node) != MA_SUCCESS) {
        ma_lpf_uninit(&f->lpf, nullptr);
        return nullptr;
    }
    return FilterPtr(f.release());
}

// Changes the cutoff: only hands the value to the audio thread (see
// AudioFilter), so a fade can update it every frame.
void setCutoff(ma_engine* engine, AudioFilter* f, float hz)
{
    if (!f) return;
    const float nyquistSafe = ma_engine_get_sample_rate(engine) * 0.45f;
    const float open = std::min(AudioSystem::kNoFilter, nyquistSafe);
    hz = std::clamp(hz, 40.0f, open);
    if (hz == f->cutoff) return;
    f->cutoff = hz;
    f->target.store(hz, std::memory_order_relaxed);
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

AudioSystem::AudioSystem() : m_music(std::make_unique<MusicPlayer>(*this)) {}

float AudioSystem::parameter(const std::string& name, float fallback) const
{
    const auto it = m_parameters.find(name);
    return it != m_parameters.end() ? it->second : fallback;
}

AudioSystem::~AudioSystem()
{
    shutdown();
}

namespace {

// miniaudio opens sound files itself (also on its streaming threads): give it
// the engine's virtual file system, so sounds play from a packaged game's
// archive like from disk (FileSystem.hpp). Read-only.
struct VfsFile {
    std::unique_ptr<std::istream> in;
};

ma_result vfsOpen(ma_vfs*, const char* path, ma_uint32 mode, ma_vfs_file* file)
{
    if (mode & MA_OPEN_MODE_WRITE) return MA_NOT_IMPLEMENTED;
    std::unique_ptr<std::istream> in = vfs::open(std::filesystem::path(path));
    if (!in) return MA_DOES_NOT_EXIST;
    *file = new VfsFile{std::move(in)};
    return MA_SUCCESS;
}

ma_result vfsOpenW(ma_vfs* v, const wchar_t* path, ma_uint32 mode, ma_vfs_file* file)
{
    return vfsOpen(v, std::filesystem::path(path).string().c_str(), mode, file);
}

ma_result vfsClose(ma_vfs*, ma_vfs_file file)
{
    delete static_cast<VfsFile*>(file);
    return MA_SUCCESS;
}

ma_result vfsRead(ma_vfs*, ma_vfs_file file, void* dst, size_t size, size_t* read)
{
    std::istream& in = *static_cast<VfsFile*>(file)->in;
    in.read(static_cast<char*>(dst), std::streamsize(size));
    const size_t got = size_t(in.gcount());
    if (read) *read = got;
    in.clear(); // (a short read at the end isn't an error)
    return got == 0 && size > 0 ? MA_AT_END : MA_SUCCESS;
}

ma_result vfsWrite(ma_vfs*, ma_vfs_file, const void*, size_t, size_t*) { return MA_NOT_IMPLEMENTED; }

ma_result vfsSeek(ma_vfs*, ma_vfs_file file, ma_int64 offset, ma_seek_origin origin)
{
    std::istream& in = *static_cast<VfsFile*>(file)->in;
    in.clear();
    in.seekg(std::streamoff(offset), origin == ma_seek_origin_start ? std::ios::beg : origin == ma_seek_origin_current ? std::ios::cur : std::ios::end);
    return in.fail() ? MA_BAD_SEEK : MA_SUCCESS;
}

ma_result vfsTell(ma_vfs*, ma_vfs_file file, ma_int64* cursor)
{
    *cursor = ma_int64(static_cast<VfsFile*>(file)->in->tellg());
    return MA_SUCCESS;
}

ma_result vfsInfo(ma_vfs*, ma_vfs_file file, ma_file_info* info)
{
    std::istream& in = *static_cast<VfsFile*>(file)->in;
    const auto at = in.tellg();
    in.seekg(0, std::ios::end);
    info->sizeInBytes = ma_uint64(in.tellg());
    in.seekg(at);
    return MA_SUCCESS;
}

ma_vfs_callbacks g_vfs{vfsOpen, vfsOpenW, vfsClose, vfsRead, vfsWrite, vfsSeek, vfsTell, vfsInfo};

} // namespace

void AudioSystem::init()
{
    auto e = std::make_unique<ma_engine>();
    ma_engine_config config = ma_engine_config_init();
    config.pResourceManagerVFS = &g_vfs; // (sound files through the virtual file system)
    if (ma_engine_init(&config, e.get()) != MA_SUCCESS) {
        log::warn("Audio: no output device available; sound disabled");
        return;
    }
    m_engine.reset(e.release()); // initialized: the deleter uninits it
    for (const char* name : {"SFX", "Ambient", "Weather", "Music", "UI"}) bus(name);
    log::info("Audio: {} Hz, {} channels", ma_engine_get_sample_rate(engine()), ma_engine_get_channels(engine()));
}

void AudioSystem::shutdown()
{
    if (!m_engine) return;
    // Sounds, then the buses they play into, then the engine.
    m_music->release();
    m_oneShots.clear();
    m_buses.clear();
    m_engine.reset();
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
        auto group = std::make_unique<ma_sound>();
        if (ma_sound_group_init(engine(), 0, nullptr, group.get()) == MA_SUCCESS) b->group.reset(group.release());
        b->filter = createFilter(engine());
        if (b->group && b->filter) {
            ma_node_attach_output_bus(b->group.get(), 0, &b->filter->node, 0);
            ma_node_attach_output_bus(&b->filter->node, 0, ma_engine_get_endpoint(engine()), 0);
        }
    }
    m_buses.push_back(std::move(b));
    return *m_buses.back();
}

void AudioSystem::setListener(const glm::vec3& p, const glm::vec3& f, const glm::vec3& u)
{
    m_listener = p;
    if (!m_engine) return;
    ma_engine_listener_set_position(engine(), 0, p.x, p.y, p.z);
    ma_engine_listener_set_direction(engine(), 0, f.x, f.y, f.z);
    ma_engine_listener_set_world_up(engine(), 0, u.x, u.y, u.z);
}

void AudioSystem::setMasterVolume(float volume)
{
    m_masterVolume = volume;
    if (m_engine) ma_engine_set_volume(engine(), volume);
}

void AudioSystem::playOneShot(const AudioClip& clip, float volume, std::optional<glm::vec3> position, float pitch,
                              const std::string& busName)
{
    OneShotSettings s;
    s.volume = volume;
    s.position = position;
    s.pitch = pitch;
    s.bus = busName;
    playOneShot(clip, s);
}

void AudioSystem::playOneShot(const AudioClip& clip, const OneShotSettings& settings)
{
    if (!m_engine) return;
    SoundPtr owned = openSound(engine(), clip.path(), MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_ASYNC, bus(settings.bus).group.get());
    if (!owned) {
        log::warn("Audio: failed to load {}", clip.path());
        return;
    }
    ma_sound* sound = owned.get();
    ma_sound_set_volume(sound, settings.volume);
    ma_sound_set_pitch(sound, settings.pitch);
    if (const auto& position = settings.position) {
        ma_sound_set_position(sound, position->x, position->y, position->z);
        if (settings.maxDistance > 0.0f) {
            ma_sound_set_attenuation_model(sound, ma_attenuation_model_linear);
            ma_sound_set_min_distance(sound, settings.minDistance);
            ma_sound_set_max_distance(sound, std::max(settings.maxDistance, settings.minDistance + 0.01f));
        }
    } else {
        ma_sound_set_spatialization_enabled(sound, MA_FALSE);
    }
    ma_sound_start(sound);
    m_oneShots.push_back(std::move(owned));
}

void AudioSystem::update(float dt)
{
    if (!m_engine) return;
    m_music->update(dt);
    std::erase_if(m_oneShots, [](const SoundPtr& s) { return ma_sound_at_end(s.get()); });

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
        if (b->group) ma_sound_group_set_volume(b->group.get(), b->volume * b->zoneVolume);
        setCutoff(engine(), b->filter.get(), std::min(b->lowpass, b->zoneLowpass));
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
    if (!m_parameters.empty() && ImGui::TreeNode("Parameters")) {
        for (auto& [name, value] : m_parameters) ImGui::DragFloat(name.c_str(), &value, 0.01f);
        ImGui::TreePop();
    }
    m_music->onGui();
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
    m_sound.reset(); // (the sound first: it feeds the filter)
    m_filter.reset();
}

void AudioSource::start()
{
    if (playOnStart) play();
}

void AudioSource::play()
{
    AudioSystem& sys = audio();
    if (!sys.available() || !clip) return;
    if (!m_sound) {
        AudioBus& b = sys.bus(bus);
        const bool filtered = occlusion && spatial;
        const ma_uint32 flags = clip->stream() ? MA_SOUND_FLAG_STREAM : MA_SOUND_FLAG_DECODE;
        // Occluded sources get their own low-pass: sound -> filter -> bus.
        m_sound = openSound(sys.engine(), clip->path(), flags, filtered ? nullptr : b.group.get());
        if (!m_sound) {
            log::warn("Audio: failed to load {}", clip->path());
            return;
        }
        if (filtered && (m_filter = createFilter(sys.engine()))) {
            ma_node_attach_output_bus(m_sound.get(), 0, &m_filter->node, 0);
            ma_node_attach_output_bus(&m_filter->node, 0, b.group ? static_cast<ma_node*>(b.group.get()) : ma_engine_get_endpoint(sys.engine()), 0);
        }
    }
    applySettings();
    const glm::vec3 p = transform().position();
    ma_sound_set_position(m_sound.get(), p.x, p.y, p.z);
    ma_sound_seek_to_pcm_frame(m_sound.get(), 0);
    ma_sound_start(m_sound.get());
}

void AudioSource::applySettings()
{
    ma_sound* s = m_sound.get();
    if (!s) return;
    ma_sound_set_volume(s, volume * (1.0f + (occlusionVolume - 1.0f) * m_occluded));
    ma_sound_set_pitch(s, pitch);
    ma_sound_set_looping(s, loop ? MA_TRUE : MA_FALSE);
    ma_sound_set_spatialization_enabled(s, spatial ? MA_TRUE : MA_FALSE);
    ma_sound_set_min_distance(s, minDistance);
    ma_sound_set_max_distance(s, maxDistance);
    ma_sound_set_attenuation_model(s, ma_attenuation_model_linear);
}

void AudioSource::stop()
{
    if (m_sound) ma_sound_stop(m_sound.get());
}

bool AudioSource::isPlaying() const
{
    return m_sound && ma_sound_is_playing(m_sound.get());
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

    ma_sound_set_volume(m_sound.get(), volume * (1.0f + (occlusionVolume - 1.0f) * m_occluded));
    setCutoff(audio().engine(), m_filter.get(), mixCutoff(AudioSystem::kNoFilter, occlusionLowpass, m_occluded));
}

void AudioSource::lateUpdate(float dt)
{
    if (!m_sound || !spatial) return;
    const glm::vec3 p = transform().position();
    ma_sound_set_position(m_sound.get(), p.x, p.y, p.z);
    if (occlusion && m_filter) updateOcclusion(dt);
}

void AudioSource::onDestroy()
{
    release();
}

} // namespace ve

// -----------------------------------------------------------------------------
// Reflection
// -----------------------------------------------------------------------------

#include "ve/scene/Reflection.hpp"
#include "ve/assets/AssetManager.hpp"
#include "ve/audio/Ambience.hpp"
#include "ve/audio/Music.hpp"

namespace ve {

// Reflection.hpp's std::shared_ptr<AudioClip> fields (stored as the clip's path).
namespace detail {
std::shared_ptr<AudioClip> loadAudioClip(const std::string& path)
{
    try {
        return Engine::get().assets().loadAudio(path);
    } catch (const std::exception&) {
        return nullptr;
    }
}
std::string audioClipPath(const std::shared_ptr<AudioClip>& clip)
{
    return clip ? clip->assetPath() : std::string();
}
} // namespace detail

void registerAudioTypes(TypeRegistry& r)
{
    r.add<AudioSource>("AudioSource", [](TypeBuilder<AudioSource>& t) {
        t.category("Audio");
        t.field("clip", &AudioSource::clip);
        t.field("bus", &AudioSource::bus);
        t.field("volume", &AudioSource::volume).range(0.0f, 2.0f).speed(0.01f);
        t.field("pitch", &AudioSource::pitch).range(0.1f, 4.0f).speed(0.01f);
        t.field("loop", &AudioSource::loop);
        t.field("playOnStart", &AudioSource::playOnStart);
        t.field("spatial", &AudioSource::spatial);
        t.field("minDistance", &AudioSource::minDistance).range(0.0f, 1000.0f);
        t.field("maxDistance", &AudioSource::maxDistance).range(0.1f, 1000.0f);
        t.field("occlusion", &AudioSource::occlusion);
        t.field("occlusionVolume", &AudioSource::occlusionVolume).range(0.0f, 1.0f).speed(0.01f);
        t.field("occlusionLowpass", &AudioSource::occlusionLowpass).range(50.0f, 20000.0f).speed(10.0f);
        t.onChanged([](AudioSource& s) { s.applySettings(); });
    });
    r.add<AudioZone>("AudioZone", [](TypeBuilder<AudioZone>& t) {
        t.category("Audio");
        t.enumField("shape", &AudioZone::shape, {"Box", "Sphere", "Cylinder"});
        t.field("halfExtents", &AudioZone::halfExtents).range(0.0f, 1000.0f);
        t.field("radius", &AudioZone::radius).range(0.0f, 1000.0f);
        t.field("height", &AudioZone::height).range(0.0f, 1000.0f);
        t.field("fadeTime", &AudioZone::fadeTime).range(0.0f, 10.0f).speed(0.01f);
        t.property(
             "effects", FieldKind::Json,
             [](const AudioZone& z) {
                 Json a = Json::array();
                 for (const auto& e : z.effects) a.push_back({{"bus", e.bus}, {"volume", e.volume}, {"lowpass", e.lowpass}});
                 return a;
             },
             [](AudioZone& z, const Json& j) {
                 if (!j.is_array()) return;
                 z.effects.clear();
                 for (const auto& e : j) {
                     AudioZone::Effect fx;
                     fx.bus = e.value("bus", std::string("Weather"));
                     fx.volume = e.value("volume", 1.0f);
                     fx.lowpass = e.value("lowpass", AudioSystem::kNoFilter);
                     z.effects.push_back(fx);
                 }
             })
            .tooltip("[{\"bus\": \"Weather\", \"volume\": 0.3, \"lowpass\": 450}]");
    });
    r.add<AmbientLayer>("AmbientLayer", [](TypeBuilder<AmbientLayer>& t) {
        t.category("Audio");
        t.field("loop", &AmbientLayer::loop).tooltip("looping bed (2D, streamed)");
        t.field("oneShots", &AmbientLayer::oneShots).tooltip("'audio/bird1.wav; audio/bird2.wav': played at random around the listener");
        t.field("interval", &AmbientLayer::interval).range(0.05f, 3600.0f).speed(0.05f).tooltip("s between one-shots (min, max) at full level");
        t.field("distance", &AmbientLayer::distance).range(0.0f, 1000.0f).tooltip("m from the listener (min, max); max 0 = 2D");
        t.field("heightRange", &AmbientLayer::heightRange).range(0.0f, 100.0f).speed(0.05f);
        t.field("pitchVariation", &AmbientLayer::pitchVariation).range(0.0f, 1.0f).speed(0.005f);
        t.field("volumeVariation", &AmbientLayer::volumeVariation).range(0.0f, 1.0f).speed(0.01f);
        t.field("volume", &AmbientLayer::volume).range(0.0f, 2.0f).speed(0.01f);
        t.field("bus", &AmbientLayer::bus);
        t.field("fromHour", &AmbientLayer::fromHour).range(0.0f, 24.0f).speed(0.05f).tooltip("plays from this hour...");
        t.field("toHour", &AmbientLayer::toHour).range(0.0f, 24.0f).speed(0.05f).tooltip("...to this one (wraps past midnight; 0..24 = always)");
        t.field("hourFade", &AmbientLayer::hourFade).range(0.0f, 12.0f).speed(0.05f).tooltip("hours to fade in / out at each end");
        t.field("biomes", &AmbientLayer::biomes).tooltip("level per terrain biome around the listener: 'Desert:1, Forest:-1' (empty = everywhere)");
        t.field("biomeRadius", &AmbientLayer::biomeRadius).range(0.0f, 500.0f).tooltip("m around the listener the biome is averaged over");
        t.field("parameters", &AmbientLayer::parameters)
            .tooltip("audio parameters: 'rain:-1, wind:-0.5' = 1 + sum (clamped); '*:0, danger:1' = only with danger");
        t.field("fadeTime", &AmbientLayer::fadeTime).range(0.0f, 60.0f).speed(0.05f);
    });
    r.add<MusicMood>("MusicMood", [](TypeBuilder<MusicMood>& t) {
        t.category("Audio");
        t.field("tracks", &MusicMood::tracks).tooltip("'audio/a.ogg; audio/b.ogg' (the object name is the mood name)");
        t.field("shuffle", &MusicMood::shuffle);
        t.field("volume", &MusicMood::volume).range(0.0f, 2.0f).speed(0.01f);
        t.field("fadeIn", &MusicMood::fadeIn).range(0.0f, 60.0f).speed(0.05f).tooltip("s to crossfade into this mood");
        t.field("gap", &MusicMood::gap).range(0.0f, 3600.0f).tooltip("s of silence between tracks (min, max)");
        t.field("playOnStart", &MusicMood::playOnStart).tooltip("the base mood when the scene starts");
        t.onChanged([](MusicMood& m) { m.apply(); });
    });
    r.add<MusicZone>("MusicZone", [](TypeBuilder<MusicZone>& t) {
        t.category("Audio");
        t.field("mood", &MusicZone::mood).tooltip("pushed while the listener is inside");
        t.field("radius", &MusicZone::radius).range(0.0f, 10000.0f);
        t.field("height", &MusicZone::height).range(0.0f, 10000.0f).tooltip("0 = sphere; >0 = vertical cylinder from the object up");
        t.field("exitMargin", &MusicZone::exitMargin).range(0.0f, 100.0f);
        t.field("priority", &MusicZone::priority).tooltip("higher wins over other pushed moods (combat: use more)");
    });
}

} // namespace ve
