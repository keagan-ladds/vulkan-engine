#include "ve/anim/Animation.hpp"

#include "ve/core/Log.hpp"
#include "ve/render/Renderer.hpp"
#include "ve/scene/Scene.hpp"

#include <glm/gtx/matrix_decompose.hpp>

#include <algorithm>
#include <cmath>

namespace ve {

// -----------------------------------------------------------------------------
// AnimationChannel
// -----------------------------------------------------------------------------

glm::vec4 AnimationChannel::sample(float t) const
{
    const size_t n = times.size();
    if (n == 0) return glm::vec4(0.0f);
    const bool cubic = interpolation == Interpolation::CubicSpline;
    auto value = [&](size_t k) { return values[cubic ? k * 3 + 1 : k]; };
    if (n == 1 || t <= times.front()) return value(0);
    if (t >= times.back()) return value(n - 1);
    const size_t k = size_t(std::upper_bound(times.begin(), times.end(), t) - times.begin()) - 1;
    const float t0 = times[k], t1 = times[k + 1], dt = t1 - t0;
    const float u = dt > 0.0f ? (t - t0) / dt : 0.0f;
    const bool rotation = path == Path::Rotation;
    switch (interpolation) {
    case Interpolation::Step: return value(k);
    case Interpolation::Linear: {
        if (!rotation) return glm::mix(value(k), value(k + 1), u);
        const glm::vec4 a = value(k), b = value(k + 1);
        const glm::quat q = glm::slerp(glm::quat(a.w, a.x, a.y, a.z), glm::quat(b.w, b.x, b.y, b.z), u);
        return {q.x, q.y, q.z, q.w};
    }
    case Interpolation::CubicSpline: {
        // Hermite: p0, m0 (out-tangent of k), p1, m1 (in-tangent of k + 1), tangents scaled by dt.
        const glm::vec4 p0 = values[k * 3 + 1], m0 = values[k * 3 + 2] * dt;
        const glm::vec4 p1 = values[(k + 1) * 3 + 1], m1 = values[(k + 1) * 3] * dt;
        const float u2 = u * u, u3 = u2 * u;
        glm::vec4 r = (2 * u3 - 3 * u2 + 1) * p0 + (u3 - 2 * u2 + u) * m0 + (-2 * u3 + 3 * u2) * p1 + (u3 - u2) * m1;
        if (rotation) r = glm::normalize(r);
        return r;
    }
    }
    return value(k);
}

// -----------------------------------------------------------------------------
// Animator
// -----------------------------------------------------------------------------

void Animator::bind(std::vector<std::shared_ptr<const AnimationClip>> clips, const std::vector<GameObject*>& nodes)
{
    m_clips = std::move(clips);
    m_nodes.assign(nodes.size(), 0);
    m_rest.assign(nodes.size(), Rest{glm::vec3(0.0f), glm::quat(1, 0, 0, 0), glm::vec3(1.0f)});
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (!nodes[i]) continue;
        m_nodes[i] = nodes[i]->id();
        const Transform& t = nodes[i]->transform();
        m_rest[i] = {t.localPosition(), t.localRotation(), t.localScale()};
    }
    std::vector<char> seen(nodes.size(), 0);
    m_animated.clear();
    for (const auto& c : m_clips)
        for (const AnimationChannel& ch : c->channels)
            if (ch.node >= 0 && size_t(ch.node) < nodes.size() && !seen[size_t(ch.node)]) {
                seen[size_t(ch.node)] = 1;
                m_animated.push_back(ch.node);
            }
    m_states.clear();
}

void Animator::start()
{
    if (playOnStart && !m_clips.empty()) play(clip.empty() ? m_clips.front()->name : clip, 0.0f);
}

void Animator::play(const std::string& name, float fade, bool loop, float clipSpeed)
{
    std::shared_ptr<const AnimationClip> c;
    for (const auto& k : m_clips)
        if (k->name == name) c = k;
    if (!c) {
        log::warnOnce("Animator on '" + gameObject().name() + "': no clip '" + name + "'");
        return;
    }
    if (!m_states.empty() && m_states.back().clip == c && m_states.back().fadeRate >= 0.0f) {
        m_states.back().loop = loop;
        m_states.back().speed = clipSpeed;
        return; // already playing (or fading in)
    }
    const float rate = fade > 0.0f ? 1.0f / fade : 1e9f;
    for (State& s : m_states) s.fadeRate = -rate; // everything else fades out
    m_states.push_back({c, 0.0f, clipSpeed, fade > 0.0f && !m_states.empty() ? 0.0f : 1.0f, rate, loop});
}

void Animator::stop(float fade)
{
    const float rate = fade > 0.0f ? 1.0f / fade : 1e9f;
    for (State& s : m_states) s.fadeRate = -rate;
}

const std::string& Animator::current() const
{
    static const std::string none;
    return m_states.empty() || m_states.back().fadeRate < 0.0f ? none : m_states.back().clip->name;
}

float Animator::time() const
{
    return m_states.empty() ? 0.0f : m_states.back().time;
}

bool Animator::finished() const
{
    return !m_states.empty() && !m_states.back().loop && m_states.back().time >= m_states.back().clip->duration;
}

std::vector<std::string> Animator::clipNames() const
{
    std::vector<std::string> names;
    for (const auto& c : m_clips) names.push_back(c->name);
    return names;
}

void Animator::update(float dt)
{
    if (m_states.empty()) return;
    for (State& s : m_states) {
        s.time += dt * speed * s.speed;
        const float d = s.clip->duration;
        if (d > 0.0f) s.time = s.loop ? std::fmod(std::max(s.time, 0.0f), d) : std::min(s.time, d);
        s.weight = std::clamp(s.weight + s.fadeRate * dt, 0.0f, 1.0f);
        if (s.fadeRate > 0.0f && s.weight >= 1.0f) s.fadeRate = 0.0f;
    }
    // Faded-out clips are done.
    m_states.erase(std::remove_if(m_states.begin(), m_states.end(), [](const State& s) { return s.fadeRate < 0.0f && s.weight <= 0.0f; }),
                   m_states.end());
    apply();
}

void Animator::apply()
{
    // Per animated node: weighted blend of every playing clip (a clip that
    // doesn't animate a channel contributes the rest pose for it).
    struct Pose {
        glm::vec3 t{0.0f}, s{0.0f};
        glm::vec4 r{0.0f};
        float w = 0.0f;
    };
    float total = 0.0f;
    for (const State& s : m_states) total += s.weight;
    if (total <= 0.0f) return;
    std::vector<Pose> poses(m_rest.size());
    for (const State& st : m_states) {
        if (st.weight <= 0.0f) continue;
        const float w = st.weight / total;
        // This clip's pose of every animated node: rest, overridden by its channels.
        for (int n : m_animated) {
            const Rest& r = m_rest[size_t(n)];
            glm::vec3 t = r.t, sc = r.s;
            glm::quat q = r.r;
            for (const AnimationChannel& ch : st.clip->channels) {
                if (ch.node != n) continue;
                const glm::vec4 v = ch.sample(st.time);
                if (ch.path == AnimationChannel::Path::Translation) t = glm::vec3(v);
                else if (ch.path == AnimationChannel::Path::Scale) sc = glm::vec3(v);
                else q = glm::quat(v.w, v.x, v.y, v.z);
            }
            Pose& p = poses[size_t(n)];
            glm::vec4 qv(q.x, q.y, q.z, q.w);
            if (p.w > 0.0f && glm::dot(qv, p.r) < 0.0f) qv = -qv; // same hemisphere before averaging
            p.t += t * w;
            p.s += sc * w;
            p.r += qv * w;
            p.w += w;
        }
    }
    Scene& scene = gameObject().scene();
    for (int n : m_animated) {
        const Pose& p = poses[size_t(n)];
        GameObject* o = m_nodes[size_t(n)] ? scene.findById(m_nodes[size_t(n)]) : nullptr;
        if (!o || p.w <= 0.0f) continue;
        const glm::vec4 q = glm::length(p.r) > 1e-6f ? glm::normalize(p.r) : glm::vec4(0, 0, 0, 1);
        o->transform().setLocalPosition(p.t / p.w);
        o->transform().setLocalRotation(glm::quat(q.w, q.x, q.y, q.z));
        o->transform().setLocalScale(p.s / p.w);
    }
}

// -----------------------------------------------------------------------------
// SkinnedMeshRenderer
// -----------------------------------------------------------------------------

void SkinnedMeshRenderer::bind(std::shared_ptr<const SkinnedMeshSource> source, const std::vector<GameObject*>& joints,
                               std::vector<glm::mat4> inverseBind)
{
    release();
    m_source = std::move(source);
    m_joints.clear();
    for (GameObject* j : joints) m_joints.push_back(j ? j->id() : 0);
    m_inverseBind = std::move(inverseBind);
    m_inverseBind.resize(m_joints.size(), glm::mat4(1.0f));
    m_system = &gameObject().renderer().skinning();
    m_instance = m_system->create(m_source);
    mesh = m_instance.mesh;
}

void SkinnedMeshRenderer::release()
{
    if (m_system) m_system->destroy(m_instance);
    m_system = nullptr;
    mesh = nullptr;
}

void SkinnedMeshRenderer::onDestroy()
{
    release();
}

SkinnedMeshRenderer::~SkinnedMeshRenderer()
{
    release();
}

void SkinnedMeshRenderer::prepareSkinning(SkinningSystem& system)
{
    if (!m_source || !m_instance.mesh || &system != m_system) return;
    Scene& scene = gameObject().scene();
    const glm::mat4 toLocal = glm::inverse(transform().worldMatrix());
    m_matrices.resize(m_joints.size());
    AABB bounds;
    for (size_t j = 0; j < m_joints.size(); ++j) {
        const GameObject* o = m_joints[j] ? scene.findById(m_joints[j]) : nullptr;
        const glm::mat4 world = o ? o->transform().worldMatrix() : transform().worldMatrix();
        m_matrices[j] = toLocal * world * m_inverseBind[j];
        // Bounds: every joint's position, padded by how far its vertices reach.
        const glm::vec3 p = glm::vec3(toLocal * world[3]);
        const float r = j < m_source->jointRadius.size() ? m_source->jointRadius[j] : 0.0f;
        bounds.expand(p - glm::vec3(r));
        bounds.expand(p + glm::vec3(r));
    }
    if (bounds.valid()) m_instance.mesh->setBounds(bounds);
    if (!system.add(m_instance, m_matrices.data(), uint32_t(m_matrices.size())))
        log::warnOnce("Skinning: more joints this frame than RendererSettings::maxSkinJoints");
}

} // namespace ve
