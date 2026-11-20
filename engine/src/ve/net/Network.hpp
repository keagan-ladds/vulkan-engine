#pragma once
// =============================================================================
// Networking (ve/net): host-authoritative co-op multiplayer.
//
// One player hosts (a listen server; a dedicated server is the same thing
// without a window) and owns the world: what exists, where, in what state.
// Clients own what they control (their character) and *ask* the host for
// everything else through RPCs; the host decides and tells everyone.
// Offline (single player) is the host with nobody connected, so game code
// written against this API runs unchanged in all three cases:
//
//   ve::net::Network& net = engine.net();          // or net() in a GameObject
//   net.host(7777);   /   net.join("192.168.1.20", 7777);
//
//   // Ask the authority (on the host / offline this runs right away, locally):
//   net.on("build.place", [&](PeerId from, Reader& r) { ... net.spawn(prefab, pos, rot); });
//   Writer w; w.writeString(prefab); w.write(pos); w.writeRotation(rot);
//   net.toServer("build.place", w);
//
//   // World objects the host creates appear on every client (late joiners too):
//   net.spawn("prefabs/avatar.prefab", pos, rot, /*owner*/ peer);
//   net.despawn(*object);
//
// Objects:
//   - NetworkObject: identity (the GameObject's id: level objects match
//     because every machine loads the same level; spawned ones get the
//     host's id) and owner (a peer; the host by default). Only the owner
//     changes its replicated state; ownerless means the host's.
//   - NetworkTransform: the owner sends position / rotation (20 Hz,
//     unreliable); everyone else interpolates ~100 ms behind.
//   - NetReplicated (interface, like ve::Saveable): netWrite / netRead for
//     custom state; call NetworkObject::markDirty() when it changes and it is
//     sent reliably at the end of the frame.
//
// Joining: the client connects, says hello (game id + protocol version), and
// the host answers with a world snapshot: by default a save-game capture
// (ve::save: moved / removed level objects, spawned prefab instances,
// every Saveable) so everything the save system covers is right for late
// joiners for free. Then come the spawned objects that aren't in saves
// (avatars), their owners and replicated state. Messages that arrive while
// the engine is loading wait until it's done (the game may rebuild the level
// before applying the snapshot).
// =============================================================================

#include "ve/net/Stream.hpp"
#include "ve/net/Transport.hpp"
#include "ve/scene/GameObject.hpp"

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ve {
class Engine;
class Scene;
using Json = nlohmann::json;
} // namespace ve

namespace ve::net {

using PeerId = uint32_t;
constexpr PeerId kServer = 0;             // the host (also "offline")
constexpr PeerId kEveryone = 0xffffffffu; // toClients target
constexpr uint16_t kDefaultPort = 7777;

enum class Role { Offline, Host, Client };
enum class Status { Offline, Hosting, Connecting, Joining, Connected, Failed };

// Implement on a GameObject or Component (with a NetworkObject on the object)
// to replicate custom state. The owner writes, everyone else reads.
class NetReplicated {
public:
    virtual ~NetReplicated() = default;
    virtual void netWrite(Writer& out) const = 0;
    virtual void netRead(Reader& in) = 0;
};

class NetworkObject : public Component {
public:
    PeerId owner() const { return m_owner; }
    bool isOwner() const;       // this machine owns it (offline: always)
    void markDirty() { m_dirty = true; } // NetReplicated state changed (owner)
    bool despawnWithOwner = true; // the owner left: the host removes it
    bool spawned() const { return m_spawned; } // created by Network::spawn (vs a level object)

private:
    friend class Network;
    PeerId m_owner = kServer;
    bool m_dirty = false;
    bool m_spawned = false;
    std::string m_prefab;
};

class NetworkTransform : public Component {
public:
    float sendRate = 20.0f;            // per second, while it moves
    float interpolationDelay = 0.1f;   // seconds behind the newest sample
    float teleportDistance = 15.0f;    // jumps further than this snap instead of sliding

    void lateUpdate(float dt) override; // others: follow the samples

private:
    friend class Network;
    struct Sample {
        double time;
        glm::vec3 position;
        glm::quat rotation;
    };
    std::deque<Sample> m_samples;
    glm::vec3 m_sentPosition{1e30f};
    glm::quat m_sentRotation{1, 0, 0, 0};
    double m_lastSend = -1e9;
};

class Network {
public:
    struct Config {
        std::string game = "ve-game"; // clients of another game are turned away
        uint32_t version = 1;         // ... and of another protocol version
        uint32_t maxPlayers = 8;      // clients, besides the host
    };
    struct PeerInfo {
        PeerId id = 0;
        std::string name;
        uint32_t roundTripMs = 0;
    };

    explicit Network(Engine& engine);
    ~Network();

    Config config;
    std::string playerName = "Player";

    // --- Session ------------------------------------------------------------------
    bool host(uint16_t port = kDefaultPort);
    bool join(const std::string& address, uint16_t port = kDefaultPort);
    void leave(); // back to offline (the host: everyone is disconnected)

    Role role() const { return m_role; }
    Status status() const { return m_status; }
    bool isServer() const { return m_role != Role::Client; } // host or offline: the authority
    bool isClient() const { return m_role == Role::Client; }
    bool online() const { return m_role != Role::Offline; }
    PeerId localPeer() const { return m_localPeer; }
    const std::vector<PeerInfo>& peers() const { return m_peerList; } // everyone in the session, the host (0) first
    const std::string& lastError() const { return m_error; }

    // --- The game's hooks ---------------------------------------------------------
    std::function<void(PeerId)> onPeerJoined; // host: a client is in (world sent)
    std::function<void(PeerId)> onPeerLeft;    // host: ... and gone (its objects despawned)
    // Host: what a joining player gets. Default: ve::save::capture(scene).
    std::function<Json(PeerId)> worldSnapshot;
    // Client: the host accepted us; apply its world. Default: ve::save::apply.
    // May start loading (rebuild the level first): later messages wait for it.
    std::function<void(const Json& world)> onJoined;
    std::function<void(const std::string& reason)> onDisconnected; // client: lost / refused / host left

    // --- Messages -------------------------------------------------------------------
    using Handler = std::function<void(PeerId from, Reader& in)>;
    void on(const std::string& name, Handler handler);
    void off(const std::string& name);
    // To the host (the authority). On the host / offline the handler runs now, from = kServer.
    void toServer(const std::string& name, const Writer& payload = {});
    // Host -> clients (one, or kEveryone). includeHost: also run it here (from = kServer).
    void toClients(const std::string& name, const Writer& payload = {}, PeerId target = kEveryone, bool includeHost = false);

    // --- World objects (host) ---------------------------------------------------------
    // Instantiate a prefab here and on every client (with the same id); it gets
    // a NetworkObject if the prefab has none. Clients can't: null (ask with toServer).
    GameObject* spawn(const std::string& prefab, const glm::vec3& position, const glm::quat& rotation = glm::quat(1, 0, 0, 0),
                      PeerId owner = kServer);
    void despawn(GameObject& object); // destroy here and on every client
    void setOwner(GameObject& object, PeerId owner);

    // --- Engine ---------------------------------------------------------------------------
    void poll();  // frame start: receive (dispatch waits while loading)
    void flush(); // frame end: send transforms / dirty state
    Transport::Stats stats(PeerId peer = kServer) const;

private:
    struct Peer {
        PeerId id;
        ConnectionId connection;
        std::string name;
        bool welcomed = false;
    };
    struct Pending {
        ConnectionId from;
        std::vector<uint8_t> data;
    };
    void onTransportEvent(const Transport::Event& e);
    void handle(ConnectionId from, std::span<const uint8_t> data);
    void handleServer(Peer& peer, uint8_t type, Reader& r, std::span<const uint8_t> raw);
    void handleClient(uint8_t type, Reader& r, std::span<const uint8_t> raw);
    void welcome(Peer& peer);
    void dropPeer(PeerId id, const std::string& why);
    void sendTo(PeerId peer, Channel ch, const Writer& w);
    void sendToClients(Channel ch, const Writer& w, PeerId except = kEveryone);
    Writer rpcMessage(const std::string& name, const Writer& payload) const;
    void dispatchRpc(PeerId from, const std::string& name, std::span<const uint8_t> payload);
    Writer spawnMessage(const NetworkObject& no) const;
    bool writeState(GameObject& o, Writer& w) const;
    void readState(GameObject& o, Reader& r);
    bool applyTransform(uint64_t id, const glm::vec3& p, const glm::quat& q, PeerId sender); // false: ignored (not theirs to move...)
    void refreshPeerList();
    void reset();
    Scene& scene() const;
    Peer* peerById(PeerId id);
    Peer* peerByConnection(ConnectionId c);
    double now() const;

    Engine& m_engine;
    Transport m_transport;
    Role m_role = Role::Offline;
    Status m_status = Status::Offline;
    PeerId m_localPeer = kServer;
    ConnectionId m_hostConnection = 0; // client: the connection to the host
    PeerId m_nextPeer = 1;
    std::vector<Peer> m_peers;
    std::vector<PeerInfo> m_peerList;
    std::vector<ConnectionId> m_sendList; // sendToClients' scratch (no allocation per message)
    std::deque<Pending> m_pending; // received while loading
    std::unordered_map<std::string, Handler> m_handlers;
    std::string m_error;
    double m_lastFlush = 0.0;
};

} // namespace ve::net

namespace ve {
class TypeRegistry;
// Reflection (scene files, editor): this module's components. The Engine calls it at startup.
void registerNetworkTypes(TypeRegistry& r);
} // namespace ve
