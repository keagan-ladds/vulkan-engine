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
// Start one with SAVANNA_NET=host[:port] or SAVANNA_NET=join:<address>[:port]
// (SAVANNA_NAME=<player name>). Known gaps (roadmap): villagers, doors,
// harvesting and pickups still run on each machine on their own.

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

// SAVANNA_NETTEST=host / client: a scripted two-process session (sandbox/tools/nettest.sh).
class NetTest : public ve::GameObject {
public:
    explicit NetTest(bool host) : GameObject("NetTest"), m_host(host)
    {
        if (!host) net().on("nettest.done", [this](ve::net::PeerId, ve::net::Reader&) { m_hostDone = true; });
    }
    void update(float dt) override;

private:
    int pieces() const;
    void log(const std::string& s) const;
    bool m_host;
    int m_step = 0;
    float m_t = 0.0f, m_total = 0.0f;
    glm::vec3 m_seen{0.0f};
    float m_moved = 0.0f;
    bool m_shot = false;
    bool m_hostDone = false;
};

} // namespace savanna
