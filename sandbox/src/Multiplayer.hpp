#pragma once

// Multiplayer (game side, on the engine's ve::net): co-op where one player
// hosts. What Savanna does with the engine's mechanisms:
//   - Every player has an avatar (prefabs/net/avatar.prefab: PlayerAvatar +
//     NetworkObject + NetworkTransform). The host spawns one per player,
//     owned by that player; the owner's copy follows its local Player and is
//     hidden, everyone else sees the villager model walking.
//   - Joining: the host's world snapshot (a save capture minus its own
//     player) is applied over the freshly built level.
//   - Building goes through the host (Builder: "build.place" /
//     "build.remove" requests), so pieces appear for everyone.
//   - The host's clock and weather are sent to clients every couple of seconds.
//
//   - Foliage changes (chopped trees, picked bushes) made on the host reach
//     everyone: the Foliage object replicates its save state.
//
// Start one with SAVANNA_NET=host[:port] or SAVANNA_NET=join:<address>[:port]
// (SAVANNA_NAME=<player name>). Known gaps (roadmap): villagers, doors,
// pickups and clients' own harvesting still run on each machine on their own.

#include <ve/net/Network.hpp>
#include <ve/scene/GameObject.hpp>
#include <ve/scene/ObjectRef.hpp>

#include <string>

namespace ve {
class Animator;
}

namespace savanna {

// Another player, as everyone else sees them.
class PlayerAvatar : public ve::GameObject, public ve::net::NetReplicated {
public:
    PlayerAvatar() : GameObject("Player avatar") {}
    void start() override;
    void lateUpdate(float dt) override;

    std::string playerName; // replicated (the owner sets it)
    void netWrite(ve::net::Writer& out) const override { out.writeString(playerName); }
    void netRead(ve::net::Reader& in) override { playerName = in.readString(); }

private:
    ve::ComponentRef<ve::Animator> m_animator; // on the model instance (a child the editor can delete)
    glm::vec3 m_last{0.0f};
    float m_speed = 0.0f;
    bool m_walking = false;
    bool m_named = false;
};

class Multiplayer : public ve::GameObject {
public:
    static constexpr const char* kAvatarPrefab = "prefabs/net/avatar.prefab";

    Multiplayer();
    void start() override;
    void update(float dt) override;
    void onDestroy() override;

    // From SAVANNA_NET / SAVANNA_NAME; true if a session was started or is running.
    static bool startFromEnvironment(ve::Engine& engine);
    PlayerAvatar* avatarOf(ve::net::PeerId peer) const;
    int avatarCount() const;

private:
    void spawnAvatar(ve::net::PeerId owner);
    float m_clockTimer = 0.0f;
};

// SAVANNA_NETTEST=host / client: a scripted two-process session (tools/nettest.sh).
// Besides building and avatars it checks the engine's session mechanisms:
// player ids, per-player saves, requests with replies (accepted / refused),
// who may send what (From), physics bodies following the host, a
// NetworkAnimator, foliage changes replicated as save state, interest
// management, lag compensation, the request budget, bandwidth numbers and kick.
class NetTest : public ve::GameObject {
public:
    explicit NetTest(bool host);
    void update(float dt) override;
    void onDestroy() override;

private:
    int pieces() const;
    void log(const std::string& s) const;
    void hostStep(float dt);
    void clientStep(float dt);
    bool m_host;
    int m_step = 0;
    float m_t = 0.0f, m_total = 0.0f;
    glm::vec3 m_seen{0.0f};
    float m_moved = 0.0f;
    bool m_shot = false;
    bool m_hostDone = false;
    // Host
    bool m_rewound = false;
    bool m_hostOnlyRan = false;
    bool m_clientDone = false;
    int m_floods = 0;
    uint64_t m_crate = 0, m_dancer = 0, m_foliage = 0;
    glm::vec3 m_near{0.0f};
    // Client
    int m_replies = 0;
    bool m_accepted = false, m_refused = false;
    std::string m_refusal;
    int m_peerChanges = 0;
    std::string m_disconnect;
    bool m_checkArrived = false, m_farArrived = false, m_nearArrived = false;
    glm::vec3 m_crateAt{0.0f}, m_far{0.0f};
    std::string m_clip;
    float m_wait = 0.0f;
    uint64_t m_receivedBytes = 0;
};

} // namespace savanna
