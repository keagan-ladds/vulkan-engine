#include "ve/anim/Animation.hpp"

#include "ve/core/Log.hpp"
#include "ve/physics/Physics.hpp"
#include "ve/render/Renderer.hpp"
#include "ve/scene/Scene.hpp"

#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/quaternion.hpp>

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
    m_nodeNames.assign(nodes.size(), {});
    m_parents.assign(nodes.size(), -1);
    m_rest.assign(nodes.size(), Rest{glm::vec3(0.0f), glm::quat(1, 0, 0, 0), glm::vec3(1.0f)});
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (!nodes[i]) continue;
        m_nodes[i] = nodes[i]->id();
        m_nodeNames[i] = nodes[i]->name();
        const Transform& t = nodes[i]->transform();
        m_rest[i] = {t.localPosition(), t.localRotation(), t.localScale()};
    }
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (!nodes[i] || !nodes[i]->transform().parent()) continue;
        const GameObject* parent = &nodes[i]->transform().parent()->gameObject();
        for (size_t j = 0; j < nodes.size(); ++j)
            if (nodes[j] == parent) m_parents[i] = int(j);
    }
    std::vector<char> seen(nodes.size(), 0);
    m_animated.clear();
    for (const auto& c : m_clips)
        for (const AnimationChannel& ch : c->channels)
            if (ch.node >= 0 && size_t(ch.node) < nodes.size() && !seen[size_t(ch.node)]) {
                seen[size_t(ch.node)] = 1;
                m_animated.push_back(ch.node);
            }
    m_layers.assign(1, Layer{"Base"});
}

void Animator::start()
{
    if (playOnStart && !m_clips.empty() && m_layers[0].states.empty()) play(clip.empty() ? m_clips.front()->name : clip, 0.0f);
}

std::shared_ptr<const AnimationClip> Animator::find(const std::string& name) const
{
    for (const auto& k : m_clips)
        if (k->name == name) return k;
    return nullptr;
}

void Animator::play(const std::string& name, float fade, bool loop, float clipSpeed)
{
    play(0, name, fade, loop, clipSpeed);
}

void Animator::play(int layer, const std::string& name, float fade, bool loop, float clipSpeed)
{
    if (layer < 0 || layer >= int(m_layers.size())) return;
    std::shared_ptr<const AnimationClip> c = find(name);
    if (!c) {
        log::warnOnce("Animator on '" + gameObject().name() + "': no clip '" + name + "'");
        return;
    }
    std::vector<State>& states = m_layers[size_t(layer)].states;
    if (!states.empty() && states.back().clip == c && states.back().fadeRate >= 0.0f) {
        states.back().loop = loop;
        states.back().speed = clipSpeed;
        return; // already playing (or fading in)
    }
    const float rate = fade > 0.0f ? 1.0f / fade : 1e9f;
    for (State& s : states) s.fadeRate = -rate; // everything else fades out
    // A layer fading in from nothing starts at full clip weight (its layer weight does the fading).
    const bool fromNothing = states.empty();
    states.push_back({c, 0.0f, clipSpeed, fade > 0.0f && !fromNothing ? 0.0f : 1.0f, rate, loop});
}

void Animator::stop(float fade)
{
    stop(0, fade);
}

void Animator::stop(int layer, float fade)
{
    if (layer < 0 || layer >= int(m_layers.size())) return;
    const float rate = fade > 0.0f ? 1.0f / fade : 1e9f;
    for (State& s : m_layers[size_t(layer)].states) s.fadeRate = -rate;
}

const std::string& Animator::current() const
{
    return current(0);
}

const std::string& Animator::current(int layer) const
{
    static const std::string none;
    if (layer < 0 || layer >= int(m_layers.size())) return none;
    const auto& st = m_layers[size_t(layer)].states;
    return st.empty() || st.back().fadeRate < 0.0f ? none : st.back().clip->name;
}

float Animator::time() const
{
    return time(0);
}

float Animator::time(int layer) const
{
    if (layer < 0 || layer >= int(m_layers.size()) || m_layers[size_t(layer)].states.empty()) return 0.0f;
    return m_layers[size_t(layer)].states.back().time;
}

bool Animator::finished() const
{
    return finished(0);
}

bool Animator::finished(int layer) const
{
    if (layer < 0 || layer >= int(m_layers.size())) return false;
    const auto& st = m_layers[size_t(layer)].states;
    return !st.empty() && !st.back().loop && st.back().time >= st.back().clip->duration;
}

std::vector<std::string> Animator::clipNames() const
{
    std::vector<std::string> names;
    for (const auto& c : m_clips) names.push_back(c->name);
    return names;
}

int Animator::addLayer(const std::string& name, const std::string& maskJoint, LayerBlend blend)
{
    if (const int i = layerIndex(name); i >= 0) return i;
    Layer l;
    l.name = name;
    l.blend = blend;
    if (!maskJoint.empty()) {
        int root = -1;
        for (size_t i = 0; i < m_nodeNames.size(); ++i)
            if (m_nodeNames[i] == maskJoint) root = int(i);
        l.mask.assign(m_nodeNames.size(), 0);
        if (root < 0) log::warn("Animator on '{}': no joint '{}' for layer '{}'", gameObject().name(), maskJoint, name);
        for (size_t i = 0; i < m_nodeNames.size() && root >= 0; ++i)
            for (int n = int(i); n >= 0; n = m_parents[size_t(n)])
                if (n == root) {
                    l.mask[i] = 1;
                    break;
                }
    }
    m_layers.push_back(std::move(l));
    return int(m_layers.size() - 1);
}

int Animator::layerIndex(const std::string& name) const
{
    for (size_t i = 0; i < m_layers.size(); ++i)
        if (m_layers[i].name == name) return int(i);
    return -1;
}

void Animator::setLayerWeight(int layer, float weight, float fade)
{
    if (layer <= 0 || layer >= int(m_layers.size())) return; // (layer 0 is always fully on)
    Layer& l = m_layers[size_t(layer)];
    l.target = std::clamp(weight, 0.0f, 1.0f);
    l.rate = fade > 0.0f ? std::abs(l.target - l.weight) / fade : 1e9f;
}

float Animator::layerWeight(int layer) const
{
    return layer >= 0 && layer < int(m_layers.size()) ? m_layers[size_t(layer)].weight : 0.0f;
}

void Animator::addEvent(const std::string& clipName, float t, const std::string& name)
{
    m_events.push_back({clipName, name, t});
}

bool Animator::animates(const GameObject& node) const
{
    for (int n : m_animated)
        if (m_nodes[size_t(n)] == node.id()) return true;
    return false;
}

void Animator::update(float dt)
{
    std::vector<std::pair<std::string, std::string>> fired; // (event, clip): sent after the update
    for (Layer& layer : m_layers) {
        // Layer weight towards its target.
        if (layer.weight != layer.target) {
            const float step = layer.rate * dt;
            layer.weight = layer.weight < layer.target ? std::min(layer.target, layer.weight + step) : std::max(layer.target, layer.weight - step);
        }
        for (size_t i = 0; i < layer.states.size(); ++i) {
            State& s = layer.states[i];
            const float before = s.time;
            s.time += dt * speed * s.speed;
            const float d = s.clip->duration;
            bool wrapped = false;
            if (d > 0.0f) {
                if (s.loop) {
                    wrapped = s.time >= d;
                    s.time = std::fmod(std::max(s.time, 0.0f), d);
                } else {
                    s.time = std::min(s.time, d);
                }
            }
            s.weight = std::clamp(s.weight + s.fadeRate * dt, 0.0f, 1.0f);
            if (s.fadeRate > 0.0f && s.weight >= 1.0f) s.fadeRate = 0.0f;
            // Events of the current clip (not one fading out) that playback just passed.
            const bool current = i + 1 == layer.states.size() && s.fadeRate >= 0.0f;
            if (!current || m_events.empty() || dt <= 0.0f) continue;
            for (const Event& e : m_events) {
                if (e.clip != s.clip->name) continue;
                const bool passed = wrapped ? (e.time > before || e.time <= s.time) : (e.time > before && e.time <= s.time);
                if (passed) fired.emplace_back(e.name, e.clip);
            }
        }
        // Faded-out clips are done.
        layer.states.erase(std::remove_if(layer.states.begin(), layer.states.end(),
                                          [](const State& s) { return s.fadeRate < 0.0f && s.weight <= 0.0f; }),
                           layer.states.end());
    }
    apply();
    if (onEvent)
        for (const auto& [event, clipName] : fired) onEvent(event, clipName);
}

float Animator::samplePose(const Layer& layer, std::vector<Pose>& poses) const
{
    poses.assign(m_rest.size(), Pose{});
    float total = 0.0f;
    for (const State& s : layer.states) total += s.weight;
    if (total <= 0.0f) return 0.0f;
    for (const State& st : layer.states) {
        if (st.weight <= 0.0f) continue;
        const float w = st.weight / total;
        // This clip's pose of every animated node: rest, overridden by its channels.
        for (int n : m_animated) {
            if (!layer.mask.empty() && !layer.mask[size_t(n)]) continue;
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
    return std::min(total, 1.0f);
}

void Animator::apply()
{
    // Layer 0: a weighted blend of its clips (a clip that doesn't animate a
    // channel contributes the rest pose for it). Then each layer on top.
    const float baseTotal = samplePose(m_layers[0], m_base);
    bool any = baseTotal > 0.0f;
    auto unpack = [](const Pose& p, glm::vec3& t, glm::quat& r, glm::vec3& s) {
        t = p.t / p.w;
        s = p.s / p.w;
        const glm::vec4 q = glm::length(p.r) > 1e-6f ? glm::normalize(p.r) : glm::vec4(0, 0, 0, 1);
        r = glm::quat(q.w, q.x, q.y, q.z);
    };
    auto pack = [](const glm::vec3& t, const glm::quat& r, const glm::vec3& s) {
        Pose p;
        p.t = t;
        p.s = s;
        p.r = glm::vec4(r.x, r.y, r.z, r.w);
        p.w = 1.0f;
        return p;
    };
    for (size_t li = 1; li < m_layers.size(); ++li) {
        const Layer& layer = m_layers[li];
        if (layer.weight <= 0.0f || layer.states.empty()) continue;
        const float total = samplePose(layer, m_scratch);
        const float lw = layer.weight * total;
        if (lw <= 0.0f) continue;
        any = true;
        for (int n : m_animated) {
            const Pose& lp = m_scratch[size_t(n)];
            if (lp.w <= 0.0f) continue;
            glm::vec3 lt, ls, bt, bs;
            glm::quat lr, br;
            unpack(lp, lt, lr, ls);
            const Rest& rest = m_rest[size_t(n)];
            if (m_base[size_t(n)].w > 0.0f) unpack(m_base[size_t(n)], bt, br, bs);
            else {
                bt = rest.t;
                br = rest.r;
                bs = rest.s;
            }
            if (layer.blend == LayerBlend::Override) {
                m_base[size_t(n)] = pack(glm::mix(bt, lt, lw), glm::slerp(br, lr, lw), glm::mix(bs, ls, lw));
            } else { // Additive: the layer's difference from the rest pose, on top
                const glm::quat dq = glm::inverse(rest.r) * lr;
                m_base[size_t(n)] = pack(bt + (lt - rest.t) * lw, br * glm::slerp(glm::quat(1, 0, 0, 0), dq, lw), bs);
            }
        }
    }
    if (!any) return;
    Scene& scene = gameObject().scene();
    for (int n : m_animated) {
        const Pose& p = m_base[size_t(n)];
        GameObject* o = m_nodes[size_t(n)] ? scene.findById(m_nodes[size_t(n)]) : nullptr;
        if (!o || p.w <= 0.0f) continue;
        glm::vec3 t, s;
        glm::quat r;
        unpack(p, t, r, s);
        o->transform().setLocalPosition(t);
        o->transform().setLocalRotation(r);
        o->transform().setLocalScale(s);
    }
}

// -----------------------------------------------------------------------------
// TwoBoneIK
// -----------------------------------------------------------------------------

namespace {
GameObject* findBelow(GameObject& root, const std::string& name)
{
    if (root.name() == name) return &root;
    for (Transform* c : root.transform().children())
        if (GameObject* f = findBelow(c->gameObject(), name)) return f;
    return nullptr;
}
} // namespace

bool TwoBoneIK::resolve()
{
    if (m_upper && m_middle && m_end) return true;
    GameObject* u = findBelow(gameObject(), upperJoint);
    GameObject* m = findBelow(gameObject(), middleJoint);
    GameObject* e = findBelow(gameObject(), endJoint);
    if (!u || !m || !e) {
        log::warnOnce("TwoBoneIK on '" + gameObject().name() + "': joints '" + upperJoint + "', '" + middleJoint + "', '" + endJoint +
                      "' not all found");
        return false;
    }
    m_upper = u->id();
    m_middle = m->id();
    m_end = e->id();
    m_restUpper = u->transform().localRotation();
    m_restMiddle = m->transform().localRotation();
    const Animator* anim = gameObject().getComponent<Animator>();
    m_resetUpper = !anim || !anim->animates(*u);
    m_resetMiddle = !anim || !anim->animates(*m);
    return true;
}

void TwoBoneIK::lateUpdate(float)
{
    if (automatic) solve();
}

bool TwoBoneIK::solve()
{
    if (!resolve()) return false;
    Scene& scene = gameObject().scene();
    GameObject* uo = scene.findById(m_upper);
    GameObject* mo = scene.findById(m_middle);
    GameObject* eo = scene.findById(m_end);
    if (!uo || !mo || !eo) return false;
    Transform& ut = uo->transform();
    Transform& mt = mo->transform();
    Transform& et = eo->transform();
    // Joints no clip animates would keep last frame's IK: start from their rest.
    if (m_resetUpper) ut.setLocalRotation(m_restUpper);
    if (m_resetMiddle) mt.setLocalRotation(m_restMiddle);
    const glm::vec3 a = ut.position(), b = mt.position(), c = et.position();
    m_animatedEnd = c;
    if (weight <= 0.0f) return true;
    const glm::quat endWorld = et.rotation();
    const glm::vec3 t = glm::mix(c, target, std::clamp(weight, 0.0f, 1.0f));

    // Where the middle joint goes: in the plane through the target and the
    // pole, at the angle the triangle's sides give (law of cosines). Then
    // turn the upper bone onto it and the lower bone onto the target.
    const float lab = glm::length(b - a), lcb = glm::length(c - b);
    if (lab < 1e-5f || lcb < 1e-5f) return false;
    glm::vec3 toT = t - a;
    const float dist = glm::length(toT);
    if (dist < 1e-5f) return false;
    const glm::vec3 d = toT / dist;
    const float lat = std::clamp(dist, std::abs(lab - lcb) + 1e-4f, (lab + lcb) * 0.9999f);
    const float cosA = std::clamp((lab * lab + lat * lat - lcb * lcb) / (2.0f * lab * lat), -1.0f, 1.0f);
    // The side the middle joint bends to: the pole (model space), or where it is now if the pole is along the limb.
    glm::vec3 side = gameObject().transform().rotation() * pole;
    side -= d * glm::dot(side, d);
    if (glm::length(side) < 1e-4f) {
        side = (b - a) - d * glm::dot(b - a, d);
        if (glm::length(side) < 1e-6f) side = std::abs(d.y) < 0.9f ? glm::cross(d, glm::vec3(0, 1, 0)) : glm::cross(d, glm::vec3(1, 0, 0));
    }
    side = glm::normalize(side);
    const glm::vec3 bNew = a + lab * (cosA * d + std::sqrt(std::max(0.0f, 1.0f - cosA * cosA)) * side);
    const glm::vec3 tReach = a + d * lat;
    auto turn = [](const glm::vec3& from, const glm::vec3& to) { // shortest arc
        const glm::vec3 f = glm::normalize(from), g = glm::normalize(to);
        const float c = glm::dot(f, g);
        if (c > 0.999999f) return glm::quat(1, 0, 0, 0);
        if (c < -0.999999f) {
            glm::vec3 axis = glm::cross(f, glm::vec3(1, 0, 0));
            if (glm::length(axis) < 1e-3f) axis = glm::cross(f, glm::vec3(0, 1, 0));
            return glm::angleAxis(3.14159265f, glm::normalize(axis));
        }
        return glm::rotation(f, g);
    };
    ut.setRotation(glm::normalize(turn(b - a, bNew - a) * ut.rotation()));
    const glm::vec3 b1 = mt.position(), c1 = et.position();
    mt.setRotation(glm::normalize(turn(c1 - b1, tReach - b1) * mt.rotation()));
    et.setRotation(endWorld); // the foot / hand keeps its animated orientation
    return true;
}

// -----------------------------------------------------------------------------
// FootIK
// -----------------------------------------------------------------------------

void FootIK::start()
{
    auto leg = [&](const std::string& u, const std::string& m, const std::string& f) {
        auto& ik = gameObject().addComponent<TwoBoneIK>(u, m, f);
        ik.automatic = false; // solved here, after the feet are placed
        ik.pole = {0.0f, 0.0f, 1.0f};
        return &ik;
    };
    m_legs[0] = leg(leftUpper, leftMiddle, leftFoot);
    m_legs[1] = leg(rightUpper, rightMiddle, rightFoot);
    if (GameObject* h = findBelow(gameObject(), hipsJoint)) m_hipsId = h->id();
}

void FootIK::lateUpdate(float dt)
{
    Scene& scene = gameObject().scene();
    GameObject* hips = m_hipsId ? scene.findById(m_hipsId) : nullptr;
    // (The legs are components on this object: still there?)
    int present = 0;
    for (const auto& c : gameObject().components()) present += c.get() == m_legs[0] || c.get() == m_legs[1];
    if (present < 2 || !hips) return;
    // Feet where the animation put them (relative to the model's ground plane), then onto the real ground.
    const glm::vec3 root = transform().position();
    QueryFilter filter;
    filter.layers = layers;
    for (GameObject* o = &gameObject(); o && !filter.ignore; o = o->transform().parent() ? &o->transform().parent()->gameObject() : nullptr)
        filter.ignore = o->getComponent<Rigidbody>(); // not our own capsule
    const float k = 1.0f - std::exp(-smoothing * dt);
    glm::vec3 feet[2];
    for (int side = 0; side < 2; ++side) {
        m_legs[side]->weight = 0.0f;
        m_legs[side]->solve(); // (resets joints no clip animates; records the animated foot)
        feet[side] = m_legs[side]->animatedEnd();
        float want = 0.0f;
        const glm::vec3 from(feet[side].x, root.y + maxStep, feet[side].z);
        if (auto hit = physics().raycast(from, {0, -1, 0}, maxStep * 2.0f, filter))
            want = std::clamp(hit->point.y - root.y, -maxStep, maxStep);
        m_offset[side] += (want - m_offset[side]) * k;
    }
    // The hips go down as far as the lower foot needs (the higher leg bends more).
    const float hipsWant = std::min({m_offset[0], m_offset[1], 0.0f}) * weight;
    m_hips += (hipsWant - m_hips) * k;
    hips->transform().setPosition(hips->transform().position() + glm::vec3(0.0f, m_hips, 0.0f));
    for (int side = 0; side < 2; ++side) {
        m_targets[side] = feet[side] + glm::vec3(0.0f, m_offset[side], 0.0f);
        m_legs[side]->target = m_targets[side];
        m_legs[side]->weight = weight;
        m_legs[side]->solve();
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
