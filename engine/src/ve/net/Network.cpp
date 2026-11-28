#include "ve/net/Network.hpp"

#include "ve/core/Engine.hpp"
#include "ve/core/Env.hpp"
#include "ve/core/Log.hpp"
#include "ve/physics/Physics.hpp"
#include "ve/scene/Components.hpp"
#include "ve/scene/Ids.hpp"
#include "ve/scene/Prefab.hpp"
#include "ve/scene/SaveGame.hpp"
#include "ve/scene/Scene.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>

namespace ve::net {

namespace {
enum Msg : uint8_t {
    kHello = 1,    // client -> host: magic, game, version, name, player id
    kWelcome,      // host -> client: your peer id, the world
    kReject,       // host -> client: why not (or why you were kicked)
    kSpawn,        // host -> clients: id, prefab, position, rotation, owner
    kDespawn,      // host -> clients: id
    kOwner,        // host -> clients: id, owner
    kTransform,    // owner -> host (and the welcome's initial poses): [id, position, rotation]...
    kState,        // owner -> host -> others: id, NetReplicated parts
    kRpc,          // either way: name, payload
    kPeerList,     // host -> clients: [id, name, player id]...
    kRequest,      // client -> host: sequence, name, payload (wants a kReply)
    kReply,        // host -> client: sequence, accepted, reason
    kView,         // client -> host: newest snapshot received (ack), camera position
    kSnapshot,     // host -> client: sequence, [id, flags, position (delta), rotation]...
    kSaveState,    // host -> clients: id, part, full / patch, JSON
    kSaveRequest,  // client -> host: id, part (a patch didn't apply: send it whole)
    kPlayerObject, // host -> clients: peer, object id (0 = none)
};
constexpr uint32_t kMagic = 0x564e4554; // "VNET"
constexpr size_t kMaxPacketBytes = 1200; // one UDP datagram on any real path (see TransformBatch)

Writer message(Msg type)
{
    Writer w;
    w.write(uint8_t(type));
    return w;
}

std::span<const uint8_t> bytesOf(const Writer& w)
{
    return {w.data().data(), w.data().size()};
}

// kTransform entries, cut into messages that fit one UDP datagram: an
// unreliable packet over the MTU gets fragmented, and ENet sends the fragments
// of an unreliable packet reliably (resends, head-of-line waits) - the opposite
// of what movement wants. ~1200 bytes stays under any real-world path MTU.
class TransformBatch {
public:
    void add(uint64_t id, const glm::vec3& p, const glm::quat& q)
    {
        m_entries.write(id);
        m_entries.write(p);
        m_entries.writeRotation(q);
        ++m_count;
    }
    bool empty() const { return m_count == 0; }
    bool full() const { return m_entries.size() >= kMaxPacketBytes; }
    // The message (count, then the entries); the batch starts over.
    Writer take()
    {
        Writer w = message(kTransform);
        w.writeVarint(m_count);
        w.append(m_entries);
        m_entries.clear();
        m_count = 0;
        return w;
    }

private:
    Writer m_entries;
    uint64_t m_count = 0;
};

// The parts of an object that replicate state: the object itself, then its components, in order.
std::vector<NetReplicated*> replicatedParts(GameObject& o)
{
    std::vector<NetReplicated*> parts;
    if (auto* r = dynamic_cast<NetReplicated*>(&o)) parts.push_back(r);
    for (const auto& c : o.components())
        if (auto* r = dynamic_cast<NetReplicated*>(c.get())) parts.push_back(r);
    return parts;
}

// The parts of an object that save games store (replicateSaveState), in the same order on every machine.
std::vector<Saveable*> saveableParts(GameObject& o)
{
    std::vector<Saveable*> parts;
    if (auto* s = dynamic_cast<Saveable*>(&o)) parts.push_back(s);
    for (const auto& c : o.components())
        if (auto* s = dynamic_cast<Saveable*>(c.get())) parts.push_back(s);
    return parts;
}

std::string saveKey(uint64_t id, size_t part)
{
    return idToHex(id) + ":" + std::to_string(part);
}

double clockSeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

// -----------------------------------------------------------------------------
// Components
// -----------------------------------------------------------------------------

bool NetworkObject::isOwner() const
{
    return m_owner == net().localPeer();
}

void NetworkTransform::followBody(bool follow)
{
    auto* rb = gameObject().getComponent<Rigidbody>();
    if (!rb) return;
    if (follow && !m_bodyTaken && rb->type == BodyType::Dynamic) {
        rb->type = BodyType::Kinematic; // follows the Transform (the owner's simulation), still pushes others
        rb->rebuild();
        m_bodyTaken = true;
    } else if (!follow && m_bodyTaken) {
        rb->type = BodyType::Dynamic; // ours to simulate again
        rb->rebuild();
        m_bodyTaken = false;
    }
}

void NetworkTransform::onDestroy()
{
    m_bodyTaken = false; // (the body goes with the object)
}

void NetworkTransform::lateUpdate(float)
{
    // The owner moves it (and Network::flush sends it); everyone else follows the samples.
    const NetworkObject* no = gameObject().getComponent<NetworkObject>();
    const bool follow = net().online() && no && !no->isOwner();
    followBody(follow);
    if (!follow || m_samples.empty()) return;
    const double t = clockSeconds() - interpolationDelay;
    while (m_samples.size() > 2 && m_samples[1].time <= t) m_samples.pop_front();
    const Sample& a = m_samples.front();
    glm::vec3 p = a.position;
    glm::quat q = a.rotation;
    if (m_samples.size() > 1 && t > a.time) {
        const Sample& b = m_samples[1];
        const float f = float(glm::clamp((t - a.time) / std::max(1e-4, b.time - a.time), 0.0, 1.0));
        p = glm::mix(a.position, b.position, f);
        q = glm::slerp(a.rotation, b.rotation, f);
    }
    transform().setPosition(p);
    transform().setRotation(q);
}

bool NetworkTransform::poseAt(double time, glm::vec3& position, glm::quat& rotation) const
{
    if (m_history.empty()) return false;
    if (time <= m_history.front().time) {
        position = m_history.front().position;
        rotation = m_history.front().rotation;
        return true;
    }
    for (size_t i = 1; i < m_history.size(); ++i) {
        const Sample& b = m_history[i];
        if (b.time < time) continue;
        const Sample& a = m_history[i - 1];
        const float f = float(glm::clamp((time - a.time) / std::max(1e-6, b.time - a.time), 0.0, 1.0));
        position = glm::mix(a.position, b.position, f);
        rotation = glm::slerp(a.rotation, b.rotation, f);
        return true;
    }
    position = m_history.back().position;
    rotation = m_history.back().rotation;
    return true;
}

// -----------------------------------------------------------------------------
// Network
// -----------------------------------------------------------------------------

Network::Network(Engine& engine) : m_engine(engine)
{
    if (const auto sim = env::get("VE_NET_SIM")) m_transport.simulate(Transport::parseSimulation(std::string(*sim)));
}

Network::~Network()
{
    m_transport.close();
}

Scene& Network::scene() const
{
    return m_engine.scene();
}

double Network::now() const
{
    return clockSeconds();
}

double Network::clock() const
{
    return clockSeconds();
}

const std::string& Network::localPlayerId()
{
    if (!playerId.empty()) return playerId;
    if (m_generatedPlayerId.empty()) {
        // Made once per user and remembered: the host knows us by it next time.
        if (auto j = save::readConfig("network"); j && j->is_object() && j->value("playerId", std::string()).size() >= 8)
            m_generatedPlayerId = (*j)["playerId"].get<std::string>();
        if (m_generatedPlayerId.empty()) {
            std::random_device rd;
            const uint64_t id = (uint64_t(rd()) << 32) ^ rd() ^ uint64_t(clockSeconds() * 1e6);
            m_generatedPlayerId = idToHex(id);
            Json j = save::readConfig("network").value_or(Json::object());
            if (!j.is_object()) j = Json::object();
            j["playerId"] = m_generatedPlayerId;
            save::writeConfig("network", j);
        }
    }
    return m_generatedPlayerId;
}

void Network::failPendingReplies(const std::string& reason)
{
    auto replies = std::move(m_replies);
    m_replies.clear();
    for (auto& [seq, reply] : replies)
        if (reply) reply(false, reason);
}

void Network::reset()
{
    const bool hadPeers = m_peerList.size() > 1;
    m_transport.close();
    m_role = Role::Offline;
    m_localPeer = kServer;
    m_hostConnection = 0;
    m_peers.clear();
    m_peerList.clear();
    m_pending.clear();
    m_playerObjects.clear();
    m_synced.clear();
    m_received = {};
    m_receivedSeq = m_ackSent = 0;
    m_bandwidth = {};
    m_lastTotalSent = m_lastTotalReceived = 0;
    m_nextPeer = 1;
    failPendingReplies("Not connected");
    if (hadPeers && onPeersChanged) onPeersChanged();
}

bool Network::host(uint16_t port)
{
    reset();
    if (!m_transport.listen(port, config.maxPlayers)) {
        m_status = Status::Failed;
        m_error = "Can't host on port " + std::to_string(port);
        return false;
    }
    m_role = Role::Host;
    m_status = Status::Hosting;
    m_lastBandwidth = now();
    refreshPeerList();
    log::info("Network: hosting on port {}", port);
    return true;
}

bool Network::join(const std::string& address, uint16_t port)
{
    reset();
    if (!m_transport.connect(address, port)) {
        m_status = Status::Failed;
        m_error = "Can't reach " + address;
        return false;
    }
    m_role = Role::Client;
    m_status = Status::Connecting;
    m_error.clear();
    m_lastBandwidth = now();
    log::info("Network: connecting to {}:{}", address, port);
    return true;
}

void Network::leave()
{
    if (m_role == Role::Offline) return;
    log::info("Network: leaving the session");
    reset();
    m_status = Status::Offline;
}

Network::Peer* Network::peerById(PeerId id)
{
    for (Peer& p : m_peers)
        if (p.id == id) return &p;
    return nullptr;
}

const Network::Peer* Network::peerById(PeerId id) const
{
    for (const Peer& p : m_peers)
        if (p.id == id) return &p;
    return nullptr;
}

Network::Peer* Network::peerByConnection(ConnectionId c)
{
    for (Peer& p : m_peers)
        if (p.connection == c) return &p;
    return nullptr;
}

const Network::PeerInfo* Network::peer(PeerId id) const
{
    for (const PeerInfo& p : m_peerList)
        if (p.id == id) return &p;
    return nullptr;
}

std::optional<PeerId> Network::findPlayer(const std::string& id) const
{
    for (const PeerInfo& p : m_peerList)
        if (p.playerId == id) return p.id;
    return std::nullopt;
}

void Network::refreshPeerList()
{
    m_peerList.clear();
    m_peerList.push_back({kServer, playerName, localPlayerId(), 0, 0.0f, 0.0f});
    for (const Peer& p : m_peers)
        if (p.welcomed) m_peerList.push_back({p.id, p.name, p.playerId, m_transport.stats(p.connection).roundTripMs, 0.0f, 0.0f});
    if (m_role == Role::Host) {
        Writer w = message(kPeerList);
        w.writeVarint(m_peerList.size());
        for (const PeerInfo& p : m_peerList) {
            w.writeVarint(p.id);
            w.writeString(p.name);
            w.writeString(p.playerId);
        }
        sendToClients(Channel::Reliable, w);
    }
    if (onPeersChanged) onPeersChanged();
}

void Network::sendTo(PeerId peer, Channel ch, const Writer& w)
{
    if (m_role == Role::Client) {
        if (peer == kServer) m_transport.send(m_hostConnection, ch, bytesOf(w));
        return;
    }
    if (Peer* p = peerById(peer); p && p->welcomed) m_transport.send(p->connection, ch, bytesOf(w));
}

void Network::sendToClients(Channel ch, const Writer& w, PeerId except)
{
    if (m_role != Role::Host) return;
    // One shared packet for everyone in the session (not still in the handshake).
    m_sendList.clear();
    for (const Peer& p : m_peers)
        if (p.welcomed && p.id != except) m_sendList.push_back(p.connection);
    if (!m_sendList.empty()) m_transport.broadcast(m_sendList, ch, bytesOf(w));
}

void Network::kick(PeerId id, const std::string& reason)
{
    if (m_role != Role::Host) return;
    Peer* p = peerById(id);
    if (!p) return;
    Writer w = message(kReject);
    w.writeString("Kicked: " + reason);
    m_transport.send(p->connection, Channel::Reliable, bytesOf(w));
    m_transport.disconnect(p->connection);
    dropPeer(id, "kicked: " + reason);
}

// --- Players ---------------------------------------------------------------------------

void Network::setPlayerObject(PeerId peer, GameObject* object)
{
    if (!isServer()) {
        log::warn("Network: only the host sets player objects");
        return;
    }
    const uint64_t id = object ? object->id() : 0;
    if (id) m_playerObjects[peer] = id;
    else m_playerObjects.erase(peer);
    Writer w = message(kPlayerObject);
    w.writeVarint(peer);
    w.write(id);
    sendToClients(Channel::Reliable, w);
}

GameObject* Network::playerObject(PeerId peer) const
{
    auto it = m_playerObjects.find(peer);
    if (it == m_playerObjects.end()) return nullptr;
    GameObject* o = scene().findById(it->second);
    return o && !o->isDestroyed() ? o : nullptr;
}

bool Network::viewpoint(PeerId peer, glm::vec3& position) const
{
    if (peer == m_localPeer) {
        if (const Camera* cam = scene().mainCamera()) {
            position = cam->transform().position();
            return true;
        }
    } else if (const Peer* p = peerById(peer); p && p->hasView) {
        position = p->view;
        return true;
    }
    if (const GameObject* o = playerObject(peer)) {
        position = o->transform().position();
        return true;
    }
    return false;
}

bool Network::withinReach(PeerId peer, const glm::vec3& point, float radius) const
{
    glm::vec3 from;
    if (const GameObject* o = playerObject(peer)) from = o->transform().position();
    else if (!viewpoint(peer, from)) return false;
    return glm::distance(from, point) <= radius;
}

// --- Messages ---------------------------------------------------------------------------

void Network::on(const std::string& name, Handler handler, From from)
{
    m_handlers[name] = {std::move(handler), from};
}

void Network::off(const std::string& name)
{
    m_handlers.erase(name);
}

Writer Network::rpcMessage(const std::string& name, const Writer& payload) const
{
    Writer w = message(kRpc);
    w.writeString(name);
    w.writeBytes(bytesOf(payload));
    return w;
}

bool Network::dispatchRpc(PeerId from, const std::string& name, std::span<const uint8_t> payload, bool remote)
{
    auto it = m_handlers.find(name);
    if (it == m_handlers.end()) {
        log::warn("Network: no handler for '{}'", name);
        return false;
    }
    if (remote) {
        // Who may send it: a client can't make the host run a host -> clients message.
        if ((isServer() && it->second.from == From::Host) || (isClient() && it->second.from == From::Clients)) {
            log::warnOnce("Network: ignored '" + name + "' from " + (isServer() ? "a client" : "the host") + " (not accepted from there)");
            return false;
        }
    }
    Reader r(payload);
    const Handler fn = it->second.fn; // (the handler may replace itself)
    if (!isServer()) {
        fn(from, r);
        return true;
    }
    // On the host every message from a client is a request the handler may reject().
    const bool outer = m_inRequest;
    m_inRequest = true;
    fn(from, r);
    m_inRequest = outer;
    return true;
}

bool Network::takeRequestToken(Peer& peer)
{
    const float rate = config.maxRequestsPerSecond;
    if (rate <= 0.0f) return true;
    const double t = now();
    peer.tokens = std::min(peer.tokens + float(t - peer.lastRefill) * rate, rate * 2.0f);
    peer.lastRefill = t;
    if (peer.tokens >= 1.0f) {
        peer.tokens -= 1.0f;
        return true;
    }
    ++m_bandwidth.droppedRequests;
    log::warnOnce("Network: '" + peer.name + "' sends more requests than Network::Config::maxRequestsPerSecond: dropped");
    return false;
}

void Network::toServer(const std::string& name, const Writer& payload)
{
    if (isServer()) {
        m_rejected = false;
        dispatchRpc(kServer, name, bytesOf(payload), false);
        m_rejected = false;
        return;
    }
    sendTo(kServer, Channel::Reliable, rpcMessage(name, payload));
}

void Network::toServer(const std::string& name, const Writer& payload, Reply reply)
{
    if (isServer()) {
        m_rejected = false;
        m_rejectReason.clear();
        const bool handled = dispatchRpc(kServer, name, bytesOf(payload), false);
        const bool ok = handled && !m_rejected;
        const std::string reason = !handled ? "Nobody handles '" + name + "'" : m_rejectReason;
        m_rejected = false;
        if (reply) reply(ok, reason);
        return;
    }
    if (m_status != Status::Connected) {
        if (reply) reply(false, "Not connected");
        return;
    }
    const uint32_t seq = m_nextRequest++;
    if (reply) m_replies[seq] = std::move(reply);
    Writer w = message(kRequest);
    w.writeVarint(seq);
    w.writeString(name);
    w.writeBytes(bytesOf(payload));
    sendTo(kServer, Channel::Reliable, w);
}

void Network::reject(const std::string& reason)
{
    if (!m_inRequest) {
        log::warnOnce("Network::reject() outside a request handler on the host: ignored");
        return;
    }
    m_rejected = true;
    m_rejectReason = reason;
}

void Network::toClients(const std::string& name, const Writer& payload, PeerId target, bool includeHost)
{
    if (!isServer()) {
        log::warn("Network: toClients('{}') from a client (ask the host with toServer)", name);
        return;
    }
    if (includeHost) dispatchRpc(kServer, name, bytesOf(payload), false);
    if (m_role != Role::Host) return;
    const Writer w = rpcMessage(name, payload);
    if (target == kEveryone) sendToClients(Channel::Reliable, w);
    else sendTo(target, Channel::Reliable, w);
}

// --- Objects ------------------------------------------------------------------------------

Writer Network::spawnMessage(const NetworkObject& no) const
{
    const GameObject& o = no.gameObject();
    Writer w = message(kSpawn);
    w.write(o.id());
    w.writeString(no.m_prefab);
    w.write(o.transform().position());
    w.writeRotation(o.transform().rotation());
    w.writeVarint(no.m_owner);
    return w;
}

GameObject* Network::spawn(const std::string& prefabPath, const glm::vec3& position, const glm::quat& rotation, PeerId owner)
{
    if (!isServer()) {
        log::warn("Network: clients can't spawn '{}' (ask the host)", prefabPath);
        return nullptr;
    }
    GameObject* o = prefab::instantiate(scene(), prefabPath, position, rotation);
    if (!o) return nullptr;
    auto* no = o->getComponent<NetworkObject>();
    if (!no) no = &o->addComponent<NetworkObject>();
    no->m_owner = owner;
    no->m_spawned = true;
    no->m_prefab = prefabPath;
    sendToClients(Channel::Reliable, spawnMessage(*no));
    return o;
}

void Network::despawn(GameObject& object)
{
    if (!isServer()) {
        log::warn("Network: clients can't despawn '{}' (ask the host)", object.name());
        return;
    }
    Writer w = message(kDespawn);
    w.write(object.id());
    sendToClients(Channel::Reliable, w);
    for (Peer& p : m_peers) {
        p.hidden.erase(object.id());
        p.baselines.erase(object.id());
        p.lastSent.erase(object.id());
    }
    object.destroy();
}

void Network::setOwner(GameObject& object, PeerId owner)
{
    if (!isServer()) return;
    auto* no = object.getComponent<NetworkObject>();
    if (!no) no = &object.addComponent<NetworkObject>();
    no->m_owner = owner;
    Writer w = message(kOwner);
    w.write(object.id());
    w.writeVarint(owner);
    sendToClients(Channel::Reliable, w);
}

bool Network::writeState(GameObject& o, Writer& w) const
{
    const auto parts = replicatedParts(o);
    if (parts.empty()) return false;
    w.write(o.id());
    w.writeVarint(parts.size());
    for (NetReplicated* p : parts) {
        Writer part;
        p->netWrite(part);
        w.writeBytes(bytesOf(part));
    }
    return true;
}

void Network::readState(GameObject& o, Reader& r)
{
    const auto parts = replicatedParts(o);
    const uint64_t n = r.readVarint();
    for (uint64_t i = 0; i < n && !r.failed(); ++i) {
        auto bytes = r.readBytes();
        if (i < parts.size() && !r.failed()) {
            Reader part(bytes);
            parts[size_t(i)]->netRead(part);
        }
    }
}

bool Network::applyTransform(uint64_t id, const glm::vec3& p, const glm::quat& q, PeerId sender)
{
    GameObject* o = scene().findById(id);
    if (!o) return false;
    const auto* no = o->getComponent<NetworkObject>();
    if (no && no->isOwner()) return false; // ours: we're the source
    if (isServer() && (!no || no->owner() != sender)) return false; // only the owner moves it
    auto* nt = o->getComponent<NetworkTransform>();
    if (!nt) {
        o->transform().setPosition(p);
        o->transform().setRotation(q);
        return true;
    }
    if (!nt->m_samples.empty() && glm::distance(nt->m_samples.back().position, p) > nt->teleportDistance) nt->m_samples.clear();
    nt->m_samples.push_back({now(), p, q});
    while (nt->m_samples.size() > 32) nt->m_samples.pop_front();
    return true;
}

// --- Lag compensation ---------------------------------------------------------------------

double Network::viewTime(PeerId peerId) const
{
    const Peer* p = peerById(peerId);
    const double halfTrip = p ? double(m_transport.stats(p->connection).roundTripMs) / 2000.0 : 0.0;
    return now() - halfTrip - 0.1; // (NetworkTransform's default interpolation delay)
}

void Network::rewind(PeerId peerId, const std::function<void()>& fn)
{
    struct Moved {
        Transform* t;
        glm::vec3 p;
        glm::quat q;
    };
    std::vector<Moved> moved;
    const Peer* p = peerById(peerId);
    const double halfTrip = p ? double(m_transport.stats(p->connection).roundTripMs) / 2000.0 : 0.0;
    const double t = now() - halfTrip;
    scene().forEach<NetworkTransform>([&](NetworkTransform& nt) {
        const auto* no = nt.gameObject().getComponent<NetworkObject>();
        if (nt.gameObject().isDestroyed() || (no && no->owner() == peerId)) return;
        glm::vec3 pos;
        glm::quat rot;
        if (!nt.poseAt(t - nt.interpolationDelay, pos, rot)) return;
        moved.push_back({&nt.transform(), nt.transform().position(), nt.transform().rotation()});
        nt.transform().setPosition(pos);
        nt.transform().setRotation(rot);
    });
    fn();
    for (const Moved& m : moved) {
        m.t->setPosition(m.p);
        m.t->setRotation(m.q);
    }
}

// --- Receiving ------------------------------------------------------------------------------

void Network::poll()
{
    if (m_role == Role::Offline) return;
    m_transport.poll([this](const Transport::Event& e) { onTransportEvent(e); });
    // What arrived while loading, in order, once it's done.
    while (!m_pending.empty() && !m_engine.loading() && m_role != Role::Offline) {
        Pending p = std::move(m_pending.front());
        m_pending.pop_front();
        handle(p.from, p.data);
    }
}

void Network::onTransportEvent(const Transport::Event& e)
{
    switch (e.type) {
    case Transport::Event::Type::Connected:
        if (m_role == Role::Client) {
            m_hostConnection = e.connection;
            m_status = Status::Joining;
            Writer w = message(kHello);
            w.write(kMagic);
            w.writeString(config.game);
            w.write(config.version);
            w.writeString(playerName);
            w.writeString(localPlayerId());
            m_transport.send(e.connection, Channel::Reliable, bytesOf(w));
        } else {
            Peer p;
            p.id = m_nextPeer++;
            p.connection = e.connection;
            m_peers.push_back(std::move(p)); // waits for its hello
        }
        break;
    case Transport::Event::Type::Disconnected:
        if (m_role == Role::Client) {
            const std::string reason = !m_error.empty()                ? m_error
                                       : m_status == Status::Connected ? "Lost the connection to the host"
                                                                       : "Couldn't connect to the host";
            log::warn("Network: {}", reason);
            m_error = reason;
            reset();
            m_status = Status::Failed;
            if (onDisconnected) onDisconnected(reason);
        } else if (Peer* p = peerByConnection(e.connection)) {
            dropPeer(p->id, "disconnected");
        }
        break;
    case Transport::Event::Type::Received:
        if (m_engine.loading() || !m_pending.empty()) m_pending.push_back({e.connection, {e.data.begin(), e.data.end()}});
        else handle(e.connection, e.data);
        break;
    }
}

void Network::handle(ConnectionId from, std::span<const uint8_t> data)
{
    Reader r(data);
    const uint8_t type = r.read<uint8_t>();
    if (r.failed()) return;
    if (m_role == Role::Client) {
        handleClient(type, r, data);
    } else if (m_role == Role::Host) {
        if (Peer* p = peerByConnection(from)) handleServer(*p, type, r, data);
    }
}

void Network::handleServer(Peer& peer, uint8_t type, Reader& r, std::span<const uint8_t>)
{
    if (!peer.welcomed && type != kHello) return; // nothing before the handshake
    switch (type) {
    case kHello: {
        const uint32_t magic = r.read<uint32_t>();
        const std::string game = r.readString();
        const uint32_t version = r.read<uint32_t>();
        peer.name = r.readString().substr(0, 32);
        peer.playerId = r.readString().substr(0, 64);
        if (peer.playerId.empty()) peer.playerId = "peer-" + std::to_string(peer.id);
        std::string refuse;
        if (r.failed() || magic != kMagic || game != config.game) refuse = "Not the same game";
        else if (version != config.version) refuse = "Different game version (host " + std::to_string(config.version) + ", you " + std::to_string(version) + ")";
        else if (peer.welcomed) return;
        else if (findPlayer(peer.playerId)) refuse = "That player is already in this session";
        if (!refuse.empty()) {
            Writer w = message(kReject);
            w.writeString(refuse);
            m_transport.send(peer.connection, Channel::Reliable, bytesOf(w));
            m_transport.disconnect(peer.connection);
            log::info("Network: refused a player: {}", refuse);
            return;
        }
        welcome(peer);
        break;
    }
    case kTransform: {
        // Moves of what this player owns: applied here; the others get them in
        // their snapshots (only the moves the host accepted: a client can't move
        // what it doesn't own on everyone else's screen either).
        const uint64_t n = r.readVarint();
        for (uint64_t i = 0; i < n && !r.failed(); ++i) {
            const auto id = r.read<uint64_t>();
            const auto p = r.read<glm::vec3>();
            const glm::quat q = r.readRotation();
            if (r.failed()) break;
            applyTransform(id, p, q, peer.id);
        }
        break;
    }
    case kState: {
        const auto id = r.read<uint64_t>();
        GameObject* o = scene().findById(id);
        auto* no = o ? o->getComponent<NetworkObject>() : nullptr;
        if (!no || no->owner() != peer.id) return;
        readState(*o, r);
        if (r.failed()) return;
        Writer relay = message(kState);
        writeState(*o, relay);
        for (Peer& other : m_peers)
            if (other.welcomed && other.id != peer.id && relevant(other, *o, *no, o->transform().position()))
                m_transport.send(other.connection, Channel::Reliable, bytesOf(relay));
        break;
    }
    case kRpc: {
        const std::string name = r.readString();
        const auto payload = r.readBytes();
        if (r.failed() || !takeRequestToken(peer)) break;
        m_rejected = false;
        dispatchRpc(peer.id, name, payload, true);
        m_rejected = false;
        break;
    }
    case kRequest: {
        const auto seq = uint32_t(r.readVarint());
        const std::string name = r.readString();
        const auto payload = r.readBytes();
        if (r.failed()) break;
        const PeerId from = peer.id; // (the handler may kick them: `peer` dangles then)
        bool ok = false;
        std::string reason;
        if (!takeRequestToken(peer)) {
            reason = "Too many requests";
        } else {
            m_rejected = false;
            m_rejectReason.clear();
            const bool handled = dispatchRpc(from, name, payload, true);
            ok = handled && !m_rejected;
            reason = !handled ? "Nobody handles '" + name + "'" : m_rejectReason;
            m_rejected = false;
        }
        Writer w = message(kReply);
        w.writeVarint(seq);
        w.write(uint8_t(ok ? 1 : 0));
        w.writeString(reason);
        sendTo(from, Channel::Reliable, w);
        break;
    }
    case kView: {
        const auto ack = uint32_t(r.readVarint());
        const bool hasView = r.read<uint8_t>() != 0;
        const auto view = r.read<glm::vec3>();
        if (r.failed()) break;
        onSnapshotAck(peer, ack);
        if (hasView) {
            peer.view = view;
            peer.hasView = true;
        }
        break;
    }
    case kSaveRequest: {
        const auto id = r.read<uint64_t>();
        const auto part = size_t(r.readVarint());
        GameObject* o = r.failed() ? nullptr : scene().findById(id);
        if (!o) break;
        const auto parts = saveableParts(*o);
        if (part >= parts.size()) break;
        Json state = Json::object();
        parts[part]->saveState(state);
        Writer w = message(kSaveState);
        w.write(id);
        w.writeVarint(part);
        w.write(uint8_t(0));
        w.writeJson(state);
        sendTo(peer.id, Channel::Reliable, w);
        break;
    }
    default: break; // (host-only messages from a client: ignored)
    }
}

void Network::welcome(Peer& peer)
{
    // Save state changed since the last flush goes to the others first, so the
    // newcomer's snapshot and everyone's patches start from the same state.
    syncSaveStates(now(), true);
    // 1. Who they are and the world (by default the save system's capture).
    Writer w = message(kWelcome);
    w.writeVarint(peer.id);
    w.writeJson(worldSnapshot ? worldSnapshot(peer.id) : save::capture(scene()));
    m_transport.send(peer.connection, Channel::Reliable, bytesOf(w));
    peer.welcomed = true;
    peer.tokens = config.maxRequestsPerSecond * 2.0f;
    peer.lastRefill = now();
    // 2. Spawned objects saves don't cover (avatars...), owners, replicated state, where things are.
    scene().forEach<NetworkObject>([&](NetworkObject& no) {
        GameObject& obj = no.gameObject();
        if (obj.isDestroyed()) return;
        const auto* pi = obj.getComponent<PrefabInstance>();
        if (no.m_spawned && !(pi && pi->saveInGame)) m_transport.send(peer.connection, Channel::Reliable, bytesOf(spawnMessage(no)));
        if (no.m_owner != kServer) {
            Writer o = message(kOwner);
            o.write(obj.id());
            o.writeVarint(no.m_owner);
            m_transport.send(peer.connection, Channel::Reliable, bytesOf(o));
        }
        Writer s = message(kState);
        if (writeState(obj, s)) m_transport.send(peer.connection, Channel::Reliable, bytesOf(s));
        if (obj.getComponent<NetworkTransform>()) {
            Writer t = message(kTransform);
            t.writeVarint(1);
            t.write(obj.id());
            t.write(obj.transform().position());
            t.writeRotation(obj.transform().rotation());
            m_transport.send(peer.connection, Channel::Reliable, bytesOf(t));
        }
    });
    // Replicated save state, whole: the starting point their patches apply to.
    scene().forEach<NetworkObject>([&](NetworkObject& no) {
        if (!no.replicateSaveState || no.m_owner != kServer || no.gameObject().isDestroyed()) return;
        const size_t parts = saveableParts(no.gameObject()).size();
        for (size_t i = 0; i < parts; ++i) {
            auto it = m_synced.find(saveKey(no.gameObject().id(), i));
            if (it == m_synced.end() || it->second.last.is_null()) continue;
            Writer sw = message(kSaveState);
            sw.write(no.gameObject().id());
            sw.writeVarint(i);
            sw.write(uint8_t(0));
            sw.writeJson(it->second.last);
            m_transport.send(peer.connection, Channel::Reliable, bytesOf(sw));
        }
    });
    for (const auto& [owner, id] : m_playerObjects) {
        Writer o = message(kPlayerObject);
        o.writeVarint(owner);
        o.write(id);
        m_transport.send(peer.connection, Channel::Reliable, bytesOf(o));
    }
    log::info("Network: '{}' joined (peer {}, player {})", peer.name, peer.id, peer.playerId);
    const PeerId id = peer.id; // (callbacks may add peers: `peer` could move)
    refreshPeerList();
    if (onPeerJoined) onPeerJoined(id);
}

void Network::dropPeer(PeerId id, const std::string& why)
{
    auto it = std::find_if(m_peers.begin(), m_peers.end(), [&](const Peer& p) { return p.id == id; });
    if (it == m_peers.end()) return;
    const bool wasIn = it->welcomed;
    const std::string name = it->name;
    m_peers.erase(it);
    if (!wasIn) return;
    log::info("Network: '{}' left ({})", name, why);
    // What they owned: gone with them, or back to the host.
    std::vector<GameObject*> owned;
    scene().forEach<NetworkObject>([&](NetworkObject& no) {
        if (!no.gameObject().isDestroyed() && no.m_owner == id) owned.push_back(&no.gameObject());
    });
    for (GameObject* o : owned) {
        if (o->getComponent<NetworkObject>()->despawnWithOwner) despawn(*o);
        else setOwner(*o, kServer);
    }
    if (m_playerObjects.count(id)) setPlayerObject(id, nullptr);
    refreshPeerList();
    if (onPeerLeft) onPeerLeft(id);
}

void Network::handleClient(uint8_t type, Reader& r, std::span<const uint8_t>)
{
    if (m_status != Status::Connected && type != kWelcome && type != kReject) return;
    switch (type) {
    case kWelcome: {
        m_localPeer = PeerId(r.readVarint());
        const Json world = r.readJson();
        if (r.failed()) {
            m_error = "Bad welcome from the host";
            leave();
            m_status = Status::Failed;
            if (onDisconnected) onDisconnected(m_error);
            return;
        }
        m_status = Status::Connected;
        log::info("Network: joined as peer {}", m_localPeer);
        if (onJoined) onJoined(world);
        else save::apply(scene(), world);
        break;
    }
    case kReject:
        m_error = r.readString();
        log::warn("Network: the host refused: {}", m_error);
        break; // (the disconnect follows)
    case kSpawn: {
        const auto id = r.read<uint64_t>();
        const std::string prefabPath = r.readString();
        const auto p = r.read<glm::vec3>();
        const glm::quat q = r.readRotation();
        const auto owner = PeerId(r.readVarint());
        if (r.failed()) return;
        GameObject* o = scene().findById(id);
        if (!o) o = prefab::instantiate(scene(), prefabPath, p, q, nullptr, id);
        if (!o) return;
        auto* no = o->getComponent<NetworkObject>();
        if (!no) no = &o->addComponent<NetworkObject>();
        no->m_owner = owner;
        no->m_spawned = true;
        no->m_prefab = prefabPath;
        break;
    }
    case kDespawn:
        if (GameObject* o = scene().findById(r.read<uint64_t>()); o && !r.failed()) o->destroy();
        break;
    case kOwner: {
        const auto id = r.read<uint64_t>();
        const auto owner = PeerId(r.readVarint());
        if (GameObject* o = scene().findById(id); o && !r.failed()) {
            auto* no = o->getComponent<NetworkObject>();
            if (!no) no = &o->addComponent<NetworkObject>();
            no->m_owner = owner;
        }
        break;
    }
    case kTransform: {
        const uint64_t n = r.readVarint();
        for (uint64_t i = 0; i < n && !r.failed(); ++i) {
            const auto id = r.read<uint64_t>();
            const auto p = r.read<glm::vec3>();
            const glm::quat q = r.readRotation();
            if (!r.failed()) applyTransform(id, p, q, kServer);
        }
        break;
    }
    case kSnapshot: readSnapshot(r); break;
    case kState: {
        const auto id = r.read<uint64_t>();
        GameObject* o = scene().findById(id);
        const auto* no = o ? o->getComponent<NetworkObject>() : nullptr;
        if (o && !(no && no->isOwner())) readState(*o, r);
        break;
    }
    case kRpc: {
        const std::string name = r.readString();
        const auto payload = r.readBytes();
        if (!r.failed()) dispatchRpc(kServer, name, payload, true);
        break;
    }
    case kReply: {
        const auto seq = uint32_t(r.readVarint());
        const bool ok = r.read<uint8_t>() != 0;
        const std::string reason = r.readString();
        if (r.failed()) break;
        auto it = m_replies.find(seq);
        if (it == m_replies.end()) break;
        Reply reply = std::move(it->second);
        m_replies.erase(it);
        if (reply) reply(ok, reason);
        break;
    }
    case kPeerList: {
        const uint64_t n = r.readVarint();
        std::vector<PeerInfo> list;
        for (uint64_t i = 0; i < n && !r.failed() && i < 256; ++i) {
            PeerInfo p;
            p.id = PeerId(r.readVarint());
            p.name = r.readString();
            p.playerId = r.readString();
            list.push_back(std::move(p));
        }
        if (!r.failed()) {
            m_peerList = std::move(list);
            if (onPeersChanged) onPeersChanged();
        }
        break;
    }
    case kPlayerObject: {
        const auto peerId = PeerId(r.readVarint());
        const auto id = r.read<uint64_t>();
        if (r.failed()) break;
        if (id) m_playerObjects[peerId] = id;
        else m_playerObjects.erase(peerId);
        break;
    }
    case kSaveState: {
        const auto id = r.read<uint64_t>();
        const auto part = size_t(r.readVarint());
        const bool full = r.read<uint8_t>() == 0;
        const Json j = r.readJson();
        GameObject* o = r.failed() ? nullptr : scene().findById(id);
        if (!o) break;
        const auto parts = saveableParts(*o);
        if (part >= parts.size()) break;
        SyncedSave& mirror = m_synced[saveKey(id, part)];
        if (full) {
            mirror.last = j;
        } else {
            if (mirror.last.is_null()) { // (first patch: we have what the snapshot gave us)
                mirror.last = Json::object();
                parts[part]->saveState(mirror.last);
            }
            try {
                mirror.last = mirror.last.patch(j);
            } catch (const std::exception&) {
                // Out of step: ask for all of it.
                mirror.last = nullptr;
                Writer w = message(kSaveRequest);
                w.write(id);
                w.writeVarint(part);
                sendTo(kServer, Channel::Reliable, w);
                break;
            }
        }
        parts[part]->loadState(mirror.last);
        break;
    }
    default: break;
    }
}

// --- Snapshots (host -> client movement) ------------------------------------------------------
//
// Each client gets its own packets: only what's relevant to it, and every
// position as a difference (millimetres) from the last pose that client
// acknowledged for that object (kView carries the newest packet it got), or
// whole when there's none. Unchanged acknowledged poses aren't sent at all.
// Rotations: 32 bits, left out when they equal the baseline's.

Network::NetPose Network::toNetPose(const glm::vec3& p, const glm::quat& q)
{
    NetPose n;
    n.position = glm::ivec3(glm::round(p * 1000.0f));
    n.rotation = packRotation(q);
    return n;
}

glm::vec3 Network::fromNetPosition(const glm::ivec3& p)
{
    return glm::vec3(p) / 1000.0f;
}

bool Network::relevant(Peer& peer, GameObject& obj, const NetworkObject& no, const glm::vec3& position)
{
    const float radius = no.relevanceRadius < 0.0f ? config.relevanceRadius : no.relevanceRadius;
    const uint64_t id = obj.id();
    const bool hidden = peer.hidden.count(id) > 0;
    glm::vec3 view;
    bool show = true;
    if (radius > 0.0f && no.owner() != peer.id && playerObject(peer.id) != &obj && viewpoint(peer.id, view)) {
        const float d = glm::distance(view, position);
        show = hidden ? d <= radius : d <= radius * 1.1f; // (a margin: no flicker at the edge)
    }
    if (show && hidden) {
        peer.hidden.erase(id);
        catchUp(peer, obj);
    } else if (!show && !hidden) {
        peer.hidden.insert(id);
    }
    return show;
}

void Network::catchUp(Peer& peer, GameObject& obj)
{
    // Back in range: its current state now; its pose goes in the next snapshot.
    Writer s = message(kState);
    if (writeState(obj, s)) m_transport.send(peer.connection, Channel::Reliable, bytesOf(s));
    peer.lastSent.erase(obj.id());
}

void Network::onSnapshotAck(Peer& peer, uint32_t seq)
{
    if (seq == 0 || seq <= peer.ackedSeq || seq > peer.snapshotSeq) return;
    peer.ackedSeq = seq;
    auto& slot = peer.sent[seq % kSnapshotRing];
    if (slot.first != seq) return; // too old: its poses were overwritten
    for (const auto& [id, pose] : slot.second) {
        Baseline& b = peer.baselines[id];
        if (seq > b.seq) b = {seq, pose};
    }
}

void Network::sendSnapshots(double t)
{
    struct Mover {
        GameObject* obj;
        NetworkObject* no;
        NetworkTransform* nt;
        glm::vec3 position;
        NetPose pose;
        double interval;
    };
    std::vector<Mover> movers;
    const double keep = std::max(0.1f, config.historySeconds);
    scene().forEach<NetworkTransform>([&](NetworkTransform& nt) {
        GameObject& obj = nt.gameObject();
        auto* no = obj.getComponent<NetworkObject>();
        if (obj.isDestroyed() || !no) return;
        // The authoritative pose: ours, or the newest the owner sent us.
        glm::vec3 p = obj.transform().position();
        glm::quat q = obj.transform().rotation();
        if (no->owner() != kServer && !nt.m_samples.empty()) {
            p = nt.m_samples.back().position;
            q = nt.m_samples.back().rotation;
        }
        // Lag compensation: where it was, for the last historySeconds.
        nt.m_history.push_back({t, p, q});
        while (nt.m_history.size() > 2 && t - nt.m_history.front().time > keep) nt.m_history.pop_front();
        movers.push_back({&obj, no, &nt, p, toNetPose(p, q), 1.0 / std::max(1.0f, nt.sendRate)});
    });
    for (Peer& peer : m_peers) {
        if (!peer.welcomed) continue;
        Writer entries;
        uint64_t count = 0;
        uint32_t seq = 0;
        const auto begin = [&] {
            seq = ++peer.snapshotSeq;
            peer.sent[seq % kSnapshotRing] = {seq, {}};
        };
        const auto emit = [&] {
            Writer w = message(kSnapshot);
            w.writeVarint(seq);
            w.writeVarint(count);
            w.append(entries);
            m_transport.send(peer.connection, Channel::Unreliable, bytesOf(w));
            entries.clear();
            count = 0;
        };
        begin();
        for (const Mover& m : movers) {
            if (m.no->owner() == peer.id) continue; // (they're the source)
            if (!relevant(peer, *m.obj, *m.no, m.position)) continue;
            const uint64_t id = m.obj->id();
            auto last = peer.lastSent.find(id);
            if (last != peer.lastSent.end() && t - last->second < m.interval) continue;
            auto b = peer.baselines.find(id);
            const bool hasBase = b != peer.baselines.end() && seq - b->second.seq < kSnapshotRing;
            if (hasBase && b->second.pose == m.pose) continue; // they have exactly this
            uint8_t flags = 0;
            if (hasBase) flags |= 1;
            if (!hasBase || b->second.pose.rotation != m.pose.rotation) flags |= 2;
            entries.write(id);
            entries.write(flags);
            if (hasBase) {
                entries.write(uint8_t(seq - b->second.seq));
                const glm::ivec3 d = m.pose.position - b->second.pose.position;
                entries.writeSigned(d.x);
                entries.writeSigned(d.y);
                entries.writeSigned(d.z);
            } else {
                entries.writeSigned(m.pose.position.x);
                entries.writeSigned(m.pose.position.y);
                entries.writeSigned(m.pose.position.z);
            }
            if (flags & 2) entries.write(m.pose.rotation);
            ++count;
            peer.sent[seq % kSnapshotRing].second.push_back({id, m.pose});
            peer.lastSent[id] = t;
            if (entries.size() >= kMaxPacketBytes) {
                emit();
                begin();
            }
        }
        if (count > 0) emit();
        else --peer.snapshotSeq; // (nothing went: the number is free again)
    }
}

void Network::readSnapshot(Reader& r)
{
    const auto seq = uint32_t(r.readVarint());
    const uint64_t n = r.readVarint();
    if (r.failed() || seq == 0) return;
    auto& slot = m_received[seq % kSnapshotRing];
    slot.first = seq;
    slot.second.clear();
    for (uint64_t i = 0; i < n && !r.failed(); ++i) {
        const auto id = r.read<uint64_t>();
        const auto flags = r.read<uint8_t>();
        NetPose pose;
        bool known = true;
        if (flags & 1) {
            const uint32_t baseSeq = seq - r.read<uint8_t>();
            glm::ivec3 d;
            d.x = int(r.readSigned());
            d.y = int(r.readSigned());
            d.z = int(r.readSigned());
            const auto& base = m_received[baseSeq % kSnapshotRing];
            const auto it = base.first == baseSeq ? base.second.find(id) : base.second.end();
            if (it == base.second.end()) {
                known = false; // (shouldn't happen: the host only uses packets we acknowledged)
            } else {
                pose.position = it->second.position + d;
                pose.rotation = it->second.rotation;
            }
        } else {
            pose.position.x = int(r.readSigned());
            pose.position.y = int(r.readSigned());
            pose.position.z = int(r.readSigned());
        }
        if (flags & 2) pose.rotation = r.read<uint32_t>();
        if (r.failed() || !known) continue;
        slot.second[id] = pose;
        applyTransform(id, fromNetPosition(pose.position), unpackRotation(pose.rotation), kServer);
    }
    if (seq > m_receivedSeq) m_receivedSeq = seq;
}

// --- Save state replication ----------------------------------------------------------------------

void Network::syncSaveStates(double t, bool force)
{
    if (m_role != Role::Host) return;
    bool anyone = false;
    for (const Peer& p : m_peers) anyone |= p.welcomed;
    if (!anyone && !force) return;
    scene().forEach<NetworkObject>([&](NetworkObject& no) {
        if (!no.replicateSaveState || no.m_owner != kServer || no.gameObject().isDestroyed()) return;
        GameObject& obj = no.gameObject();
        const auto parts = saveableParts(obj);
        for (size_t i = 0; i < parts.size(); ++i) {
            SyncedSave& s = m_synced[saveKey(obj.id(), i)];
            const uint64_t rev = parts[i]->saveRevision();
            if (!force && !s.last.is_null()) {
                if (rev != 0 && rev == s.revision) continue;     // unchanged (tracked)
                if (rev == 0 && t - s.lastCheck < 1.0) continue; // untracked: look once a second
            }
            s.lastCheck = t;
            s.revision = rev;
            Json state = Json::object();
            parts[i]->saveState(state);
            if (!s.last.is_null() && state == s.last) continue;
            // A patch when it's smaller (most changes), else the whole state.
            Json payload;
            bool full = s.last.is_null();
            if (!full) {
                payload = Json::diff(s.last, state);
                if (payload.dump().size() > state.dump().size()) full = true;
            }
            if (full) payload = state;
            s.last = std::move(state);
            if (!anyone) continue;
            Writer w = message(kSaveState);
            w.write(obj.id());
            w.writeVarint(i);
            w.write(uint8_t(full ? 0 : 1));
            w.writeJson(payload);
            // (Everyone: relevance doesn't apply - save state is world state, like terrain.)
            sendToClients(Channel::Reliable, w);
        }
    });
}

// --- Sending ---------------------------------------------------------------------------------

void Network::updateBandwidth(double t)
{
    const double dt = t - m_lastBandwidth;
    if (dt < 1.0) return;
    m_lastBandwidth = t;
    uint64_t sent = 0, received = 0;
    if (m_role == Role::Client) {
        const auto s = m_transport.stats(m_hostConnection);
        sent = s.bytesSent;
        received = s.bytesReceived;
        if (!m_peerList.empty()) m_peerList[0].roundTripMs = s.roundTripMs;
    } else {
        for (Peer& p : m_peers) {
            const auto s = m_transport.stats(p.connection);
            sent += s.bytesSent;
            received += s.bytesReceived;
            for (PeerInfo& info : m_peerList) {
                if (info.id != p.id) continue;
                info.roundTripMs = s.roundTripMs;
                info.sentPerSecond = float(double(s.bytesSent - std::min(s.bytesSent, p.lastBytesSent)) / dt);
                info.receivedPerSecond = float(double(s.bytesReceived - std::min(s.bytesReceived, p.lastBytesReceived)) / dt);
            }
            p.lastBytesSent = s.bytesSent;
            p.lastBytesReceived = s.bytesReceived;
        }
    }
    // (Totals since the session started, even as connections come and go.)
    m_bandwidth.sentPerSecond = float(double(sent >= m_lastTotalSent ? sent - m_lastTotalSent : sent) / dt);
    m_bandwidth.receivedPerSecond = float(double(received >= m_lastTotalReceived ? received - m_lastTotalReceived : received) / dt);
    m_bandwidth.sent += sent >= m_lastTotalSent ? sent - m_lastTotalSent : sent;
    m_bandwidth.received += received >= m_lastTotalReceived ? received - m_lastTotalReceived : received;
    m_lastTotalSent = sent;
    m_lastTotalReceived = received;
}

void Network::flush()
{
    const bool live = (m_role == Role::Host && !m_peers.empty()) || (m_role == Role::Client && m_status == Status::Connected);
    const double t = now();
    if (m_role != Role::Offline) updateBandwidth(t);
    if (!live || m_engine.loading()) {
        m_transport.flush();
        return;
    }
    if (m_role == Role::Host) {
        sendSnapshots(t);
        syncSaveStates(t, false);
        // Dirty replicated state, to whoever it's relevant to.
        scene().forEach<NetworkObject>([&](NetworkObject& no) {
            GameObject& obj = no.gameObject();
            if (obj.isDestroyed() || !no.isOwner() || !no.m_dirty) return;
            no.m_dirty = false;
            Writer s = message(kState);
            if (!writeState(obj, s)) return;
            for (Peer& peer : m_peers)
                if (peer.welcomed && relevant(peer, obj, no, obj.transform().position()))
                    m_transport.send(peer.connection, Channel::Reliable, bytesOf(s));
        });
        if (t - m_lastFlush > 2.0) { // round trips for the player list now and then
            m_lastFlush = t;
            for (PeerInfo& p : m_peerList)
                if (Peer* peer = peerById(p.id)) p.roundTripMs = m_transport.stats(peer->connection).roundTripMs;
        }
        m_transport.flush();
        return;
    }

    // Client: what we own moves / changes; acknowledge snapshots and say where we look.
    TransformBatch moves;
    scene().forEach<NetworkObject>([&](NetworkObject& no) {
        GameObject& obj = no.gameObject();
        if (obj.isDestroyed() || !no.isOwner()) return;
        if (auto* nt = obj.getComponent<NetworkTransform>(); nt && t - nt->m_lastSend >= 1.0 / std::max(1.0f, nt->sendRate)) {
            const glm::vec3 p = obj.transform().position();
            const glm::quat q = obj.transform().rotation();
            const bool moved = glm::distance(p, nt->m_sentPosition) > 0.002f || std::abs(glm::dot(q, nt->m_sentRotation)) < 0.99999f;
            if (moved || t - nt->m_lastSend > 1.0) { // (a slow heartbeat while still)
                moves.add(obj.id(), p, q);
                if (moves.full()) sendTo(kServer, Channel::Unreliable, moves.take());
                nt->m_sentPosition = p;
                nt->m_sentRotation = q;
                nt->m_lastSend = t;
            }
        }
        if (no.m_dirty) {
            no.m_dirty = false;
            Writer s = message(kState);
            if (writeState(obj, s)) sendTo(kServer, Channel::Reliable, s);
        }
    });
    if (!moves.empty()) sendTo(kServer, Channel::Unreliable, moves.take());
    const bool newSnapshot = m_receivedSeq != m_ackSent;
    if ((newSnapshot && t - m_lastView >= 0.05) || t - m_lastView >= 1.0 / std::max(0.5f, config.viewSendRate)) {
        m_lastView = t;
        m_ackSent = m_receivedSeq;
        Writer w = message(kView);
        w.writeVarint(m_receivedSeq);
        const Camera* cam = scene().mainCamera();
        w.write(uint8_t(cam ? 1 : 0));
        w.write(cam ? cam->transform().position() : glm::vec3(0.0f));
        sendTo(kServer, Channel::Unreliable, w);
    }
    m_transport.flush();
}

Transport::Stats Network::stats(PeerId peerId) const
{
    if (m_role == Role::Client) return m_transport.stats(m_hostConnection);
    for (const Peer& p : m_peers)
        if (p.id == peerId) return m_transport.stats(p.connection);
    return {};
}

} // namespace ve::net

// -----------------------------------------------------------------------------
// Reflection
// -----------------------------------------------------------------------------

#include "ve/net/NetworkAnimator.hpp"
#include "ve/scene/Reflection.hpp"

namespace ve {

void registerNetworkTypes(TypeRegistry& r)
{
    r.add<net::NetworkObject>("NetworkObject", [](TypeBuilder<net::NetworkObject>& t) {
        t.category("Network");
        t.field("despawnWithOwner", &net::NetworkObject::despawnWithOwner).tooltip("the owner left: the host removes it (else the host takes it over)");
        t.field("relevanceRadius", &net::NetworkObject::relevanceRadius)
            .range(-1.0f, 5000.0f)
            .tooltip("only players whose view is this close (m) get its movement and state; 0 = everyone; -1 = the session's default");
        t.field("replicateSaveState", &net::NetworkObject::replicateSaveState)
            .tooltip("also send its save-game state (Saveable) from the host when it changes");
    });
    r.add<net::NetworkTransform>("NetworkTransform", [](TypeBuilder<net::NetworkTransform>& t) {
        t.category("Network");
        t.field("sendRate", &net::NetworkTransform::sendRate).range(1.0f, 60.0f);
        t.field("interpolationDelay", &net::NetworkTransform::interpolationDelay).range(0.0f, 0.5f).speed(0.005f);
        t.field("teleportDistance", &net::NetworkTransform::teleportDistance).range(0.5f, 100.0f);
    });
    r.add<net::NetworkAnimator>("NetworkAnimator", [](TypeBuilder<net::NetworkAnimator>& t) {
        t.category("Network");
        t.field("fade", &net::NetworkAnimator::fade).range(0.0f, 1.0f).speed(0.01f).tooltip("crossfade (s) when the clip changes");
    });
}

} // namespace ve
