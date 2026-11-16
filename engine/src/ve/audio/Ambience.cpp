#include "ve/audio/Ambience.hpp"

#include "ve/assets/AssetManager.hpp"
#include "ve/audio/Audio.hpp"
#include "ve/core/Log.hpp"
#include "ve/scene/Scene.hpp"
#include "ve/world/Terrain.hpp"
#include "ve/world/Vegetation.hpp"

#include <miniaudio.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>

namespace ve {

namespace {

void warnOnce(const std::string& message)
{
    static std::set<std::string> said;
    if (said.insert(message).second) log::warn("{}", message);
}

float smooth01(float edge0, float edge1, float x)
{
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

float AmbientLayer::hourWeight(float hour, float from, float to, float fade)
{
    const float len = to - from;
    if (std::abs(len) >= 24.0f || std::abs(len) < 1e-4f) return 1.0f; // all day
    const float span = std::fmod(len + 48.0f, 24.0f);                 // window length (wraps past midnight)
    const float x = std::fmod(hour - from + 48.0f, 24.0f);            // hours since the window opened
    // Signed distance to the nearest edge: positive inside.
    const float d = x <= span ? std::min(x, span - x) : -std::min(x - span, 24.0f - x);
    if (fade <= 1e-4f) return d >= 0.0f ? 1.0f : 0.0f;
    return smooth01(-0.5f * fade, 0.5f * fade, d);
}

void AmbientLayer::start()
{
    m_rng.seed(static_cast<uint32_t>(std::hash<std::string>{}(gameObject().name())));
    m_level = 0.0f;
    m_target = computeTarget();
    m_level = m_target; // start at the right level instead of fading in on load
}

float AmbientLayer::computeTarget()
{
    const AudioSystem& sys = audio();
    float t = std::max(volume, 0.0f) > 0.0f ? 1.0f : 0.0f;

    if (sys.hasParameter("timeOfDay")) t *= hourWeight(sys.parameter("timeOfDay"), fromHour, toHour, hourFade);

    if (!parameters.empty() && t > 0.0f) {
        float base = 1.0f, sum = 0.0f;
        if (parameters != m_paramsSpec) {
            m_paramsSpec = parameters;
            m_params = parseNameValues(parameters);
        }
        for (const auto& [name, k] : m_params) {
            if (name == "*") base = k;
            else sum += k * sys.parameter(name);
        }
        t *= std::clamp(base + sum, 0.0f, 1.0f);
    }

    if (!biomes.empty() && t > 0.0f) {
        const Terrain* terrain = Terrain::find(gameObject().scene());
        const std::shared_ptr<const TerrainData> td = terrain ? terrain->data() : nullptr;
        if (td) {
            if (biomes != m_biomeSpec || terrain->version() != m_biomeVersion) {
                m_biomeSpec = biomes;
                m_biomeVersion = terrain->version();
                m_biomeWeights = parseBiomeWeights(biomes, *td, "AmbientLayer '" + gameObject().name() + "'");
            }
            // The listener and four points around it: borders crossfade over ~biomeRadius.
            const glm::vec3 l = sys.listenerPosition();
            const glm::vec2 c(l.x, l.z);
            const float r = std::max(biomeRadius, 0.0f);
            float b = biomeDensity(m_biomeWeights, *td, c) * 2.0f;
            for (const glm::vec2 o : {glm::vec2(r, 0), glm::vec2(-r, 0), glm::vec2(0, r), glm::vec2(0, -r)})
                b += biomeDensity(m_biomeWeights, *td, c + o);
            t *= b / 6.0f;
        }
    }
    return t;
}

void AmbientLayer::update(float dt)
{
    AudioSystem& sys = audio();
    m_target = computeTarget();
    const float k = 1.0f - std::exp(-3.0f * dt / std::max(fadeTime, 0.01f));
    m_level += (m_target - m_level) * k;
    if (m_target <= 0.0f && m_level < 1e-3f) m_level = 0.0f;
    if (!sys.available()) return;

    // ---- Loop ----
    static const std::string kNoLoop;
    const std::string& loopPath = loop ? loop->path() : kNoLoop;
    if (loopPath != m_loopPath) releaseLoop();
    if (loop && m_level > 1e-3f && !m_loop) {
        m_loop = new ma_sound();
        if (ma_sound_init_from_file(sys.engine(), loop->path().c_str(), MA_SOUND_FLAG_STREAM, sys.bus(bus).group, nullptr,
                                    m_loop) != MA_SUCCESS) {
            warnOnce("AmbientLayer: failed to load " + loop->path());
            delete m_loop;
            m_loop = nullptr;
        } else {
            m_loopPath = loopPath;
            ma_sound_set_spatialization_enabled(m_loop, MA_FALSE);
            ma_sound_set_looping(m_loop, MA_TRUE);
            ma_sound_set_volume(m_loop, 0.0f);
            // Start somewhere in the loop so layers sharing a file don't phase.
            ma_uint64 frames = 0;
            if (ma_sound_get_length_in_pcm_frames(m_loop, &frames) == MA_SUCCESS && frames > 0)
                ma_sound_seek_to_pcm_frame(m_loop, std::uniform_int_distribution<ma_uint64>(0, frames - 1)(m_rng));
            ma_sound_start(m_loop);
        }
    }
    if (m_loop) {
        if (m_level <= 0.0f) releaseLoop();
        else ma_sound_set_volume(m_loop, volume * m_level);
    }

    // ---- One-shots ----
    if (oneShots != m_shotsSpec) {
        m_shotsSpec = oneShots;
        m_shots.clear();
        std::stringstream ss(oneShots);
        std::string item;
        while (std::getline(ss, item, ';')) {
            item.erase(0, item.find_first_not_of(" \t\n"));
            item.erase(item.find_last_not_of(" \t\n") + 1);
            if (item.empty()) continue;
            try {
                m_shots.push_back(assets().loadAudio(item));
            } catch (const std::exception&) {
                warnOnce("AmbientLayer '" + gameObject().name() + "': sound not found: " + item);
            }
        }
    }
    if (m_shots.empty() || m_level <= 0.0f) return;
    auto rand01 = [&] { return std::uniform_real_distribution<float>(0.0f, 1.0f)(m_rng); };
    const float lo = std::max(0.05f, std::min(interval.x, interval.y));
    const float hi = std::max(lo, std::max(interval.x, interval.y));
    if (m_shotTimer < 0.0f) m_shotTimer = lo + (hi - lo) * rand01();
    // Lower levels stretch the interval rather than just playing quieter.
    m_shotTimer -= dt * m_level;
    if (m_shotTimer > 0.0f) return;
    m_shotTimer = lo + (hi - lo) * rand01();

    OneShotSettings s;
    s.bus = bus;
    s.volume = volume * std::sqrt(m_level) * (1.0f - volumeVariation * rand01());
    s.pitch = 1.0f + pitchVariation * (rand01() * 2.0f - 1.0f);
    if (distance.y > 0.0f) {
        const float a = rand01() * 6.2831853f;
        const float d = distance.x + (distance.y - distance.x) * rand01();
        s.position = sys.listenerPosition() + glm::vec3(std::cos(a) * d, rand01() * heightRange, std::sin(a) * d);
        s.minDistance = std::max(distance.x, 1.0f);
        s.maxDistance = std::max(distance.y, distance.x) * 3.0f;
    }
    sys.playOneShot(*m_shots[std::uniform_int_distribution<size_t>(0, m_shots.size() - 1)(m_rng)], s);
}

void AmbientLayer::releaseLoop()
{
    if (m_loop) {
        ma_sound_uninit(m_loop);
        delete m_loop;
        m_loop = nullptr;
    }
    m_loopPath.clear();
}

void AmbientLayer::onDestroy()
{
    releaseLoop();
}

AmbientLayer::~AmbientLayer()
{
    releaseLoop();
}

} // namespace ve
