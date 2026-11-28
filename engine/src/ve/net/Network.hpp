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
//
// Mechanisms, not rules: the engine never decides what a request means, who
// may do what or what a player is. It gives the game:
//   - identity: every player has a stable playerId (kept across sessions)
//     besides the per-session PeerId; a player object and a viewpoint;
//   - requests with an answer (toServer + Reply; the host's handler may
//     reject()), so a client can predict a change and undo it if refused;
//     handlers say who may call them (From); a per-client request budget;
//   - replication: NetworkTransform (bodies a machine doesn't own follow
//     kinematically), NetReplicated, NetworkAnimator (ve/net/NetworkAnimator.hpp)
//     and any ve::Saveable's state (NetworkObject::replicateSaveState: sent
//     as JSON patches when it changes, e.g. Foliage, Terrain edits);
//   - scale: interest management (relevanceRadius around each player's
//     viewpoint), host -> client transforms delta-encoded against what that
//     client acknowledged, bandwidth numbers, lag compensation (pose history,
//     viewTime, rewind), a bad-network simulator (VE_NET_SIM).
// =============================================================================

#include "ve/net/Stream.hpp"
#include "ve/net/Transport.hpp"
#include "ve/scene/GameObject.hpp"

#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ve {
class Engine;
class Scene;
using Json = nlohmann::json;
} // namespace ve

namespace ve::net {

/// Identifies a machine in a session: kServer (the host), clients from 1.
using PeerId = uint32_t;
constexpr PeerId kServer = 0;             ///< the host (also "offline")
constexpr PeerId kEveryone = 0xffffffffu; ///< toClients target
/// The UDP port host() / join() use unless told otherwise.
constexpr uint16_t kDefaultPort = 7777;

/// What this machine is in the session.
enum class Role { Offline, Host, Client };
/// Connection progress (Network::status()).
enum class Status { Offline, Hosting, Connecting, Joining, Connected, Failed };
/// Which machines a message handler accepts messages from (Network::on).
/// Messages this machine sends itself (toServer on the host, toClients with
/// includeHost) always run.
enum class From {
    Anyone,  ///< Clients (requests to the host) and the host (messages to clients).
    Clients, ///< Only requests from clients, handled on the host (a client calling toClients is ignored anyway).
    Host,    ///< Only messages from the host, handled on clients: a client can't make the host run it.
};

/// Implement on a GameObject or Component (with a NetworkObject on the object)
/// to replicate custom state. The owner writes, everyone else reads.
class NetReplicated {
public:
    virtual ~NetReplicated() = default;
    /// Owner: writes the replicated state into `out` (sent when markDirty() was called).
    virtual void netWrite(Writer& out) const = 0;
    /// Everyone else: reads what netWrite wrote, in the same order.
    virtual void netRead(Reader& in) = 0;
};

/// Makes its object exist on every machine of a session (with the same id):
/// who owns it, and replication of its NetReplicated state.
class NetworkObject : public Component {
public:
    /// The peer that owns (simulates) it; kServer for the host's objects.
    PeerId owner() const { return m_owner; }
    bool isOwner() const;       ///< this machine owns it (offline: always)
    void markDirty() { m_dirty = true; } ///< NetReplicated state changed (owner)
    bool despawnWithOwner = true; ///< the owner left: the host removes it
    bool spawned() const { return m_spawned; } ///< created by Network::spawn (vs a level object)
    /// Interest management: the host only sends this object's movement and
    /// state to players whose viewpoint is within this many metres (catching
    /// them up when they come near). 0 = everyone, always; negative = the
    /// session's Network::Config::relevanceRadius.
    float relevanceRadius = -1.0f;
    /// Also replicate the state of this object's ve::Saveable parts (what
    /// save games store: Foliage's chopped trees, Terrain edits, the game's
    /// own Saveables) from the host to clients when it changes, as JSON
    /// patches. Host-owned objects only.
    bool replicateSaveState = false;

private:
    friend class Network;
    PeerId m_owner = kServer;
    bool m_dirty = false;
    bool m_spawned = false;
    std::string m_prefab;
};

/// Replicates its object's position and rotation from the owner to everyone
/// else, interpolated. A dynamic Rigidbody on a machine that doesn't own the
/// object is switched to kinematic while online (it follows the owner's
/// simulation and still pushes others), and back when this machine owns it.
class NetworkTransform : public Component {
public:
    float sendRate = 20.0f;            ///< per second, while it moves
    float interpolationDelay = 0.1f;   ///< seconds behind the newest sample
    float teleportDistance = 15.0f;    ///< jumps further than this snap instead of sliding

    void lateUpdate(float dt) override; ///< others: follow the samples
    void onDestroy() override;          ///< gives a taken-over body back its type

    /// Host: where the object was at `time` (seconds on Network::clock()),
    /// from the last Network::Config::historySeconds; for lag compensation.
    /// False if there's no history (offline, or too long ago: the oldest is used).
    bool poseAt(double time, glm::vec3& position, glm::quat& rotation) const;

private:
    friend class Network;
    struct Sample {
        double time;
        glm::vec3 position;
        glm::quat rotation;
    };
    void followBody(bool follow); ///< a dynamic body becomes kinematic while it follows samples
    std::deque<Sample> m_samples;
    std::deque<Sample> m_history; ///< host: authoritative poses for poseAt
    glm::vec3 m_sentPosition{1e30f};
    glm::quat m_sentRotation{1, 0, 0, 0};
    double m_lastSend = -1e9;
    bool m_bodyTaken = false; ///< the Rigidbody was made kinematic by us
};

/// Multiplayer sessions (host / join over UDP, ENet): peers, named messages,
/// networked objects. Engine::net(); offline until host() or join().
/// The host is the authority; offline behaves like a host with no clients.
class Network {
public:
    /// Session settings (set before host() / join()).
    struct Config {
        std::string game = "ve-game"; ///< clients of another game are turned away
        uint32_t version = 1;         ///< ... and of another protocol version
        uint32_t maxPlayers = 8;      ///< clients, besides the host
        /// Requests (toServer) a client may send per second, on average; a
        /// burst of twice that is allowed. Over it: dropped (a Reply gets
        /// "too many requests"). 0 = no limit.
        float maxRequestsPerSecond = 60.0f;
        /// NetworkObjects with a negative relevanceRadius use this (metres; 0 = everyone, always).
        float relevanceRadius = 0.0f;
        /// How often clients tell the host where their camera is (per second): interest management.
        float viewSendRate = 5.0f;
        /// Seconds of pose history the host keeps per NetworkTransform (lag compensation).
        float historySeconds = 1.0f;
    };
    /// One machine in the session.
    struct PeerInfo {
        PeerId id = 0;  ///< Its peer id for this session (kServer = the host).
        std::string name;  ///< The player name it gave.
        std::string playerId; ///< Its stable player id (the same every time this player joins).
        uint32_t roundTripMs = 0;  ///< Latency to it in milliseconds (host: to each client; client: to the host).
        float sentPerSecond = 0.0f;     ///< Host: bytes per second we send it (averaged over a second).
        float receivedPerSecond = 0.0f; ///< Host: bytes per second it sends us.
    };
    /// Whole-session traffic of this machine (bandwidth()).
    struct Bandwidth {
        float sentPerSecond = 0.0f;     ///< Bytes per second sent, all connections (averaged over a second).
        float receivedPerSecond = 0.0f; ///< Bytes per second received.
        uint64_t sent = 0;              ///< Bytes sent since the session started.
        uint64_t received = 0;          ///< Bytes received since the session started.
        uint32_t droppedRequests = 0;   ///< Host: requests dropped by the per-client budget.
    };
    /// The answer to a request (toServer with a Reply): accepted, or the reason it was refused.
    using Reply = std::function<void(bool accepted, const std::string& reason)>;

    /// Made by the Engine.
    explicit Network(Engine& engine);
    ~Network();

    Config config;  ///< Game / version / player limits (set before host() or join()).
    std::string playerName = "Player";  ///< This player's name, sent when joining.
    /// This player's stable id, sent when joining: the host knows a returning
    /// player by it (their saved character, inventory...). Empty = a random id
    /// made once and kept in the save folder's config ("network").
    std::string playerId;
    /// The player id in effect (playerId, or the remembered random one).
    const std::string& localPlayerId();

    // --- Session ------------------------------------------------------------------
    /// Starts hosting a session on `port` (UDP). False (lastError()) if the port can't be opened.
    bool host(uint16_t port = kDefaultPort);
    /// Starts joining the session at `address` (host name or IP); watch status() / onJoined. False if it can't start.
    bool join(const std::string& address, uint16_t port = kDefaultPort);
    void leave(); ///< back to offline (the host: everyone is disconnected)

    /// Offline, Host or Client.
    Role role() const { return m_role; }
    /// Connection progress.
    Status status() const { return m_status; }
    bool isServer() const { return m_role != Role::Client; } ///< host or offline: the authority
    /// Whether this machine joined someone else's session.
    bool isClient() const { return m_role == Role::Client; }
    /// Whether a session is running (host or client).
    bool online() const { return m_role != Role::Offline; }
    /// This machine's peer id (kServer on the host and offline).
    PeerId localPeer() const { return m_localPeer; }
    const std::vector<PeerInfo>& peers() const { return m_peerList; } ///< everyone in the session, the host (0) first
    /// One peer of peers() (null if not in the session).
    const PeerInfo* peer(PeerId id) const;
    /// The peer playing as `playerId` (nullopt if they aren't in the session).
    std::optional<PeerId> findPlayer(const std::string& playerId) const;
    /// Why the last host / join / connection failed.
    const std::string& lastError() const { return m_error; }
    /// Host: disconnects a client, telling them why (their onDisconnected gets "Kicked: <reason>").
    void kick(PeerId peer, const std::string& reason);

    /// The object a peer plays as (their character), for validation and as
    /// their viewpoint when they don't report a camera. Host: set it (it's
    /// sent to everyone); clients read it. Null = none / gone.
    void setPlayerObject(PeerId peer, GameObject* object);
    /// The object `peer` plays as (null if none).
    GameObject* playerObject(PeerId peer) const;
    /// Host: where `peer` is looking from (their camera, reported a few times
    /// a second; else their player object). False if unknown.
    bool viewpoint(PeerId peer, glm::vec3& position) const;
    /// Host: whether `point` is within `radius` metres of `peer`'s player
    /// object (or viewpoint if they have none). For validating requests
    /// ("can they reach that?"); the radius is the game's rule.
    bool withinReach(PeerId peer, const glm::vec3& point, float radius) const;

    // --- The game's hooks ---------------------------------------------------------
    std::function<void(PeerId)> onPeerJoined; ///< host: a client is in (world sent)
    std::function<void(PeerId)> onPeerLeft;    ///< host: ... and gone (its objects despawned)
    /// Every machine: peers() changed (someone joined, left, renamed); for player lists.
    std::function<void()> onPeersChanged;
    /// Host: what a joining player gets. Default: ve::save::capture(scene).
    std::function<Json(PeerId)> worldSnapshot;
    /// Client: the host accepted us; apply its world. Default: ve::save::apply.
    /// May start loading (rebuild the level first): later messages wait for it.
    std::function<void(const Json& world)> onJoined;
    std::function<void(const std::string& reason)> onDisconnected; ///< client: lost / refused / host left

    // --- Messages -------------------------------------------------------------------
    /// A message handler: `from` = the sender, `in` = the payload.
    using Handler = std::function<void(PeerId from, Reader& in)>;
    /// Handles messages called `name` (replacing an earlier handler for it).
    /// @param from which machines may send it (messages from others are ignored).
    void on(const std::string& name, Handler handler, From from = From::Anyone);
    /// Stops handling messages called `name`.
    void off(const std::string& name);
    /// To the host (the authority). On the host / offline the handler runs now, from = kServer.
    void toServer(const std::string& name, const Writer& payload = {});
    /// A request with an answer: `reply` runs on this machine when the host
    /// has handled it (accepted unless its handler called reject(); refused
    /// if nobody handles it, the budget ran out or the connection dropped).
    /// On the host / offline it runs right after the handler. Use it to
    /// predict: apply the change locally now, undo it if refused.
    void toServer(const std::string& name, const Writer& payload, Reply reply);
    /// Inside a handler on the host: refuses the request being handled (its
    /// Reply gets accepted = false and `reason`).
    void reject(const std::string& reason);
    /// Host -> clients (one, or kEveryone). includeHost: also run it here (from = kServer).
    void toClients(const std::string& name, const Writer& payload = {}, PeerId target = kEveryone, bool includeHost = false);

    // --- World objects (host) ---------------------------------------------------------
    /// Instantiate a prefab here and on every client (with the same id); it gets
    /// a NetworkObject if the prefab has none. Clients can't: null (ask with toServer).
    GameObject* spawn(const std::string& prefab, const glm::vec3& position, const glm::quat& rotation = glm::quat(1, 0, 0, 0),
                      PeerId owner = kServer);
    void despawn(GameObject& object); ///< destroy here and on every client
    /// Host: hands an object to another peer (it simulates and replicates it from then on).
    void setOwner(GameObject& object, PeerId owner);

    // --- Lag compensation (host) -------------------------------------------------------
    /// The network clock (seconds, steady): the time base of poseAt / viewTime.
    double clock() const;
    /// Host: the moment `peer` is seeing now, on clock(): half its round trip
    /// plus the interpolation delay ago. A shot they fire hit what was there then.
    double viewTime(PeerId peer) const;
    /// Host: runs `fn` with every NetworkTransform object (except the ones
    /// `peer` owns) moved to where `peer` saw it (viewTime), then puts them
    /// back. Transforms only: hit-test against them (shapes, bones), not
    /// against physics bodies, which stay where they are.
    void rewind(PeerId peer, const std::function<void()>& fn);

    // --- Engine ---------------------------------------------------------------------------
    void poll();  ///< frame start: receive (dispatch waits while loading)
    void flush(); ///< frame end: send transforms / dirty state
    /// Connection statistics to a peer (round trip, bytes).
    Transport::Stats stats(PeerId peer = kServer) const;
    /// This machine's traffic (all connections).
    const Bandwidth& bandwidth() const { return m_bandwidth; }
    /// Bad network conditions for testing (VE_NET_SIM sets them at startup).
    void simulate(const Transport::Simulation& sim) { m_transport.simulate(sim); }
    /// The connections underneath (statistics, the simulation in effect).
    const Transport& transport() const { return m_transport; }

private:
    /// A pose as snapshots carry it (millimetres, packed rotation): host and
    /// client keep exactly the same values for the delta baselines.
    struct NetPose {
        glm::ivec3 position{0};
        uint32_t rotation = 0;
        bool operator==(const NetPose&) const = default;
    };
    static constexpr uint32_t kSnapshotRing = 64; ///< snapshot packets remembered for acks / baselines
    struct Baseline {
        uint32_t seq = 0;
        NetPose pose;
    };
    struct Peer {
        PeerId id;
        ConnectionId connection;
        std::string name;
        std::string playerId;
        bool welcomed = false;
        // Requests budget.
        float tokens = 0.0f;
        double lastRefill = 0.0;
        // Interest management.
        glm::vec3 view{0.0f};
        bool hasView = false;
        std::unordered_set<uint64_t> hidden; ///< objects out of its range (not sent)
        // Snapshots: sent packets (for acks), acked baselines, send times.
        uint32_t snapshotSeq = 0;
        uint32_t ackedSeq = 0;
        std::array<std::pair<uint32_t, std::vector<std::pair<uint64_t, NetPose>>>, kSnapshotRing> sent{};
        std::unordered_map<uint64_t, Baseline> baselines;
        std::unordered_map<uint64_t, double> lastSent;
        // Bandwidth (bytes per second over the last second).
        uint64_t lastBytesSent = 0, lastBytesReceived = 0;
    };
    struct Pending {
        ConnectionId from;
        std::vector<uint8_t> data;
    };
    struct HandlerEntry {
        Handler fn;
        From from = From::Anyone;
    };
    /// A Saveable part replicated from the host (replicateSaveState).
    struct SyncedSave {
        Json last;             ///< what clients have (host) / the mirror (client)
        uint64_t revision = 0; ///< Saveable::saveRevision() when last sent
        double lastCheck = -1e9;
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
    /// Runs a handler; `remote`: the message came over the network from `from`.
    bool dispatchRpc(PeerId from, const std::string& name, std::span<const uint8_t> payload, bool remote);
    bool takeRequestToken(Peer& peer);
    Writer spawnMessage(const NetworkObject& no) const;
    bool writeState(GameObject& o, Writer& w) const;
    void readState(GameObject& o, Reader& r);
    bool applyTransform(uint64_t id, const glm::vec3& p, const glm::quat& q, PeerId sender); ///< false: ignored (not theirs to move...)
    void refreshPeerList();
    void reset();
    void failPendingReplies(const std::string& reason);
    Scene& scene() const;
    Peer* peerById(PeerId id);
    const Peer* peerById(PeerId id) const;
    Peer* peerByConnection(ConnectionId c);
    double now() const;
    // Host: per-client snapshots of moving objects, interest, Saveable sync.
    bool relevant(Peer& peer, GameObject& obj, const NetworkObject& no, const glm::vec3& position);
    void catchUp(Peer& peer, GameObject& obj);
    void sendSnapshots(double t);
    void syncSaveStates(double t, bool force);
    void onSnapshotAck(Peer& peer, uint32_t seq);
    void readSnapshot(Reader& r);
    void updateBandwidth(double t);
    static NetPose toNetPose(const glm::vec3& p, const glm::quat& q);
    static glm::vec3 fromNetPosition(const glm::ivec3& p);

    Engine& m_engine;
    Transport m_transport;
    Role m_role = Role::Offline;
    Status m_status = Status::Offline;
    PeerId m_localPeer = kServer;
    ConnectionId m_hostConnection = 0; ///< client: the connection to the host
    PeerId m_nextPeer = 1;
    std::vector<Peer> m_peers;
    std::vector<PeerInfo> m_peerList;
    std::vector<ConnectionId> m_sendList; ///< sendToClients' scratch (no allocation per message)
    std::deque<Pending> m_pending; ///< received while loading
    std::unordered_map<std::string, HandlerEntry> m_handlers;
    std::string m_error;
    std::string m_generatedPlayerId;
    double m_lastFlush = 0.0;
    // Requests with replies.
    uint32_t m_nextRequest = 1;
    std::unordered_map<uint32_t, Reply> m_replies; ///< client: waiting for the host
    bool m_inRequest = false;                      ///< a handler is running for a request
    std::string m_rejectReason;
    bool m_rejected = false;
    // Player objects (peer -> object id) and the client's own view reports / snapshot acks.
    std::unordered_map<PeerId, uint64_t> m_playerObjects;
    double m_lastView = -1e9;
    uint32_t m_receivedSeq = 0, m_ackSent = 0;
    std::array<std::pair<uint32_t, std::unordered_map<uint64_t, NetPose>>, kSnapshotRing> m_received{};
    // Saveable sync, keyed by save key (object id + part).
    std::unordered_map<std::string, SyncedSave> m_synced;
    // Traffic.
    Bandwidth m_bandwidth;
    uint64_t m_lastTotalSent = 0, m_lastTotalReceived = 0;
    double m_lastBandwidth = 0.0;
};

} // namespace ve::net

namespace ve {
class TypeRegistry;
/// Reflection (scene files, editor): this module's components. The Engine calls it at startup.
void registerNetworkTypes(TypeRegistry& r);
} // namespace ve
