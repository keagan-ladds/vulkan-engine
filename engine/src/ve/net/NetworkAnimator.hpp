#pragma once
// =============================================================================
// NetworkAnimator - replicates an Animator's playback (the clip on each
// layer, looping, clip speed, layer weights, overall speed) from the
// object's owner to everyone else.
//
//   // On a networked object (NetworkObject + NetworkTransform) whose model
//   // has an Animator (on the object or any child):
//   obj.addComponent<ve::net::NetworkAnimator>();
//   // The owner just plays clips as usual; the others follow:
//   animator.play("Wave");          // ...appears on every machine
//
// What it sends is what the Animator is doing, not why: the game's own
// animation logic (state machines, speed -> walk) runs only on the owner.
// Layers are matched by index, so every machine must add the same layers
// (addLayer) in the same order, as code that builds the character does.
// Changes are sent when they happen (reliably, like any NetReplicated);
// clip times aren't kept in step (a late joiner starts the current clip from
// its beginning).
// =============================================================================

#include "ve/net/Network.hpp"
#include "ve/scene/ObjectRef.hpp"

#include <string>
#include <vector>

namespace ve {
class Animator;
}

namespace ve::net {

/// Replicates the playback of the Animator on its object (or a child) from
/// the owner to everyone else. Needs a NetworkObject on the same object.
class NetworkAnimator : public Component, public NetReplicated {
public:
    float fade = 0.2f; ///< Crossfade (seconds) the others use when the clip on a layer changes.

    void lateUpdate(float dt) override; ///< owner: notices changes; others: apply what arrived
    void netWrite(Writer& out) const override;
    void netRead(Reader& in) override;

    /// The Animator it drives / watches (on this object or the first child that has one; null if none).
    Animator* animator();

private:
    /// What one layer is doing.
    struct LayerState {
        std::string clip;  // "" = nothing
        bool loop = true;
        float clipSpeed = 1.0f;
        float weight = 1.0f; // target weight
        bool operator==(const LayerState&) const = default;
    };
    /// The Animator's state now.
    void read(Animator& a, float& speed, std::vector<LayerState>& layers) const;

    ComponentRef<Animator> m_animator;
    float m_speed = 1.0f;
    std::vector<LayerState> m_layers; ///< owner: last sent; others: last received
    bool m_received = false;          ///< others: apply m_layers on the next lateUpdate
};

} // namespace ve::net
