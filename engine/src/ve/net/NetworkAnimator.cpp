#include "ve/net/NetworkAnimator.hpp"

#include "ve/anim/Animation.hpp"
#include "ve/scene/Scene.hpp"

#include <cmath>

namespace ve::net {

Animator* NetworkAnimator::animator()
{
    if (Animator* a = m_animator.get(gameObject().scene())) return a;
    Animator* a = gameObject().getComponentInChildren<Animator>(); // (the model is often a child)
    m_animator = a;
    return a;
}

void NetworkAnimator::read(Animator& a, float& speed, std::vector<LayerState>& layers) const
{
    speed = std::round(a.speed * 100.0f) / 100.0f;
    layers.resize(size_t(a.layerCount()));
    for (int i = 0; i < a.layerCount(); ++i) {
        LayerState& l = layers[size_t(i)];
        l.clip = a.current(i);
        l.loop = a.looping(i);
        l.clipSpeed = std::round(a.clipSpeed(i) * 100.0f) / 100.0f; // (rounded: tiny drifts aren't changes)
        l.weight = std::round(a.layerWeightTarget(i) * 100.0f) / 100.0f;
    }
}

void NetworkAnimator::lateUpdate(float)
{
    Animator* a = animator();
    NetworkObject* no = gameObject().getComponent<NetworkObject>();
    if (!a || !no) return;
    if (no->isOwner()) {
        // Ours: send when what the Animator does changes.
        float speed = 1.0f;
        std::vector<LayerState> layers;
        read(*a, speed, layers);
        if (speed != m_speed || layers != m_layers) {
            m_speed = speed;
            m_layers = std::move(layers);
            no->markDirty();
        }
        return;
    }
    if (!m_received) return;
    // Theirs: do what the owner's Animator does (the layers this machine has).
    m_received = false;
    a->speed = m_speed;
    for (int i = 0; i < a->layerCount() && i < int(m_layers.size()); ++i) {
        const LayerState& l = m_layers[size_t(i)];
        if (l.clip.empty()) {
            if (!a->current(i).empty()) a->stop(i, fade);
        } else if (a->current(i) != l.clip || a->looping(i) != l.loop || a->clipSpeed(i) != l.clipSpeed) {
            a->play(i, l.clip, fade, l.loop, l.clipSpeed);
        }
        if (i > 0 && std::abs(a->layerWeightTarget(i) - l.weight) > 0.005f) a->setLayerWeight(i, l.weight, fade);
    }
}

void NetworkAnimator::netWrite(Writer& out) const
{
    out.write(m_speed);
    out.writeVarint(m_layers.size());
    for (const LayerState& l : m_layers) {
        out.writeString(l.clip);
        out.write(uint8_t(l.loop ? 1 : 0));
        out.write(l.clipSpeed);
        out.write(l.weight);
    }
}

void NetworkAnimator::netRead(Reader& in)
{
    const float speed = in.read<float>();
    const uint64_t n = in.readVarint();
    std::vector<LayerState> layers;
    for (uint64_t i = 0; i < n && i < 64 && !in.failed(); ++i) {
        LayerState l;
        l.clip = in.readString();
        l.loop = in.read<uint8_t>() != 0;
        l.clipSpeed = in.read<float>();
        l.weight = in.read<float>();
        layers.push_back(std::move(l));
    }
    if (in.failed()) return;
    m_speed = speed;
    m_layers = std::move(layers);
    m_received = true;
}

} // namespace ve::net
