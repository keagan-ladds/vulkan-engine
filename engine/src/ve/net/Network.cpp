#include "ve/net/Network.hpp"

#include "ve/core/Engine.hpp"
#include "ve/core/Log.hpp"
#include "ve/scene/Prefab.hpp"
#include "ve/scene/SaveGame.hpp"
#include "ve/scene/Scene.hpp"

#include <algorithm>
#include <chrono>

namespace ve::net {

namespace {
enum Msg : uint8_t {
    kHello = 1,  // client -> host: magic, game, version, name
    kWelcome,    // host -> client: your peer id, the world
    kReject,     // host -> client: why not
    kSpawn,      // host -> clients: id, prefab, position, rotation, owner
    kDespawn,    // host -> clients: id
    kOwner,      // host -> clients: id, owner
    kTransform,  // owner -> host -> others: [id, position, rotation]...
    kState,      // owner -> host -> others: id, NetReplicated parts
    kRpc,        // either way: name, payload
    kPeerList,   // host -> clients: [id, name]...
};
constexpr uint32_t kMagic = 0x564e4554; // "VNET"

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
    static constexpr size_t kMaxBytes = 1200;

    void add(uint64_t id, const glm::vec3& p, const glm::quat& q)
    {
        m_entries.write(id);
        m_entries.write(p);
        m_entries.writeRotation(q);
        ++m_count;
    }
    bool empty() const { return m_count == 0; }
    bool full() const { return m_entries.size() >= kMaxBytes; }
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
} // namespace

// -----------------------------------------------------------------------------
// Components
// -----------------------------------------------------------------------------

bool NetworkObject::isOwner() const
{
    return m_owner == net().localPeer();
}

void NetworkTransform::lateUpdate(float)
{
    // The owner moves it (and Network::flush sends it); everyone else follows the samples.
    const NetworkObject* no = gameObject().getComponent<NetworkObject>();
    if (!net().online() || !no || no->isOwner() || m_samples.empty()) return;
    const auto clock = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const double t = clock - interpolationDelay;
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

// -----------------------------------------------------------------------------
// Network
// -----------------------------------------------------------------------------

Network::Network(Engine& engine) : m_engine(engine) {}

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
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void Network::reset()
{
    m_transport.close();
    m_role = Role::Offline;
    m_localPeer = kServer;
    m_hostConnection = 0;
    m_peers.clear();
    m_peerList.clear();
    m_pending.clear();
    m_nextPeer = 1;
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

Network::Peer* Network::peerByConnection(ConnectionId c)
{
    for (Peer& p : m_peers)
        if (p.connection == c) return &p;
    return nullptr;
}

void Network::refreshPeerList()
{
    m_peerList.clear();
    m_peerList.push_back({kServer, playerName, 0});
    for (const Peer& p : m_peers)
        if (p.welcomed) m_peerList.push_back({p.id, p.name, m_transport.stats(p.connection).roundTripMs});
    if (m_role != Role::Host) return;
    Writer w = message(kPeerList);
    w.writeVarint(m_peerList.size());
    for (const PeerInfo& p : m_peerList) {
        w.writeVarint(p.id);
        w.writeString(p.name);
    }
    sendToClients(Channel::Reliable, w);
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

// --- Messages ---------------------------------------------------------------------------

void Network::on(const std::string& name, Handler handler)
{
    m_handlers[name] = std::move(handler);
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

void Network::dispatchRpc(PeerId from, const std::string& name, std::span<const uint8_t> payload)
{
    auto it = m_handlers.find(name);
    if (it == m_handlers.end()) {
        log::warn("Network: no handler for '{}'", name);
        return;
    }
    Reader r(payload);
    it->second(from, r);
}

void Network::toServer(const std::string& name, const Writer& payload)
{
    if (isServer()) {
        dispatchRpc(kServer, name, bytesOf(payload));
        return;
    }
    sendTo(kServer, Channel::Reliable, rpcMessage(name, payload));
}

void Network::toClients(const std::string& name, const Writer& payload, PeerId target, bool includeHost)
{
    if (!isServer()) {
        log::warn("Network: toClients('{}') from a client (ask the host with toServer)", name);
        return;
    }
    if (includeHost) dispatchRpc(kServer, name, bytesOf(payload));
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
            m_transport.send(e.connection, Channel::Reliable, bytesOf(w));
        } else {
            m_peers.push_back({m_nextPeer++, e.connection, {}, false}); // waits for its hello
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
        std::string refuse;
        if (r.failed() || magic != kMagic || game != config.game) refuse = "Not the same game";
        else if (version != config.version) refuse = "Different game version (host " + std::to_string(config.version) + ", you " + std::to_string(version) + ")";
        else if (peer.welcomed) return;
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
        // Moves of what this player owns: apply here, pass on to the others.
        // Only the moves the host accepted are relayed: a client can't move
        // what it doesn't own on everyone else's screen either.
        const uint64_t n = r.readVarint();
        TransformBatch relay;
        for (uint64_t i = 0; i < n && !r.failed(); ++i) {
            const auto id = r.read<uint64_t>();
            const auto p = r.read<glm::vec3>();
            const glm::quat q = r.readRotation();
            if (r.failed()) break;
            if (!applyTransform(id, p, q, peer.id)) continue;
            relay.add(id, p, q);
            if (relay.full()) sendToClients(Channel::Unreliable, relay.take(), peer.id);
        }
        if (!relay.empty()) sendToClients(Channel::Unreliable, relay.take(), peer.id);
        break;
    }
    case kState: {
        const auto id = r.read<uint64_t>();
        GameObject* o = scene().findById(id);
        const auto* no = o ? o->getComponent<NetworkObject>() : nullptr;
        if (!no || no->owner() != peer.id) return;
        readState(*o, r);
        if (r.failed()) return;
        Writer relay = message(kState);
        writeState(*o, relay);
        sendToClients(Channel::Reliable, relay, peer.id);
        break;
    }
    case kRpc: {
        const std::string name = r.readString();
        const auto payload = r.readBytes();
        if (!r.failed()) dispatchRpc(peer.id, name, payload);
        break;
    }
    default: break; // (host-only messages from a client: ignored)
    }
}

void Network::welcome(Peer& peer)
{
    // 1. Who they are and the world (by default the save system's capture).
    Writer w = message(kWelcome);
    w.writeVarint(peer.id);
    w.writeJson(worldSnapshot ? worldSnapshot(peer.id) : save::capture(scene()));
    m_transport.send(peer.connection, Channel::Reliable, bytesOf(w));
    peer.welcomed = true;
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
    log::info("Network: '{}' joined (peer {})", peer.name, peer.id);
    refreshPeerList();
    if (onPeerJoined) onPeerJoined(peer.id);
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
        if (!r.failed()) dispatchRpc(kServer, name, payload);
        break;
    }
    case kPeerList: {
        const uint64_t n = r.readVarint();
        std::vector<PeerInfo> list;
        for (uint64_t i = 0; i < n && !r.failed() && i < 256; ++i) {
            PeerInfo p;
            p.id = PeerId(r.readVarint());
            p.name = r.readString();
            list.push_back(std::move(p));
        }
        if (!r.failed()) m_peerList = std::move(list);
        break;
    }
    default: break;
    }
}

// --- Sending ---------------------------------------------------------------------------------

void Network::flush()
{
    const bool live = (m_role == Role::Host && !m_peers.empty()) || (m_role == Role::Client && m_status == Status::Connected);
    if (!live || m_engine.loading()) {
        m_transport.flush();
        return;
    }
    const double t = now();
    const auto send = [&](Channel ch, const Writer& w) {
        if (m_role == Role::Host) sendToClients(ch, w);
        else sendTo(kServer, ch, w);
    };
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
                if (moves.full()) send(Channel::Unreliable, moves.take());
                nt->m_sentPosition = p;
                nt->m_sentRotation = q;
                nt->m_lastSend = t;
            }
        }
        if (no.m_dirty) {
            no.m_dirty = false;
            Writer s = message(kState);
            if (writeState(obj, s)) send(Channel::Reliable, s);
        }
    });
    if (!moves.empty()) send(Channel::Unreliable, moves.take());
    if (m_role == Role::Host && t - m_lastFlush > 2.0) { // round trips for the player list now and then
        m_lastFlush = t;
        for (PeerInfo& p : m_peerList)
            if (Peer* peer = peerById(p.id)) p.roundTripMs = m_transport.stats(peer->connection).roundTripMs;
    }
    m_transport.flush();
}

Transport::Stats Network::stats(PeerId peer) const
{
    if (m_role == Role::Client) return m_transport.stats(m_hostConnection);
    for (const Peer& p : m_peers)
        if (p.id == peer) return m_transport.stats(p.connection);
    return {};
}

} // namespace ve::net
