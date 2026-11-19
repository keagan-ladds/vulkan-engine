#include "ve/net/Transport.hpp"

#include "ve/core/Log.hpp"

#include <enet/enet.h>

#include <mutex>
#include <unordered_map>

namespace ve::net {

namespace {
int g_users = 0;
std::mutex g_initMutex;

bool initEnet()
{
    std::lock_guard lock(g_initMutex);
    if (g_users++ == 0 && enet_initialize() != 0) {
        g_users = 0;
        log::error("Network: ENet failed to initialize");
        return false;
    }
    return true;
}

void releaseEnet()
{
    std::lock_guard lock(g_initMutex);
    if (g_users > 0 && --g_users == 0) enet_deinitialize();
}

constexpr size_t kChannels = 2;
} // namespace

struct Transport::Impl {
    ENetHost* host = nullptr;
    bool initialized = false;
    ConnectionId nextId = 1;
    std::unordered_map<ConnectionId, ENetPeer*> peers;

    ConnectionId idOf(ENetPeer* p) { return ConnectionId(reinterpret_cast<uintptr_t>(p->data)); }
    ENetPacket* packet(Channel ch, std::span<const uint8_t> bytes)
    {
        const uint32_t flags = ch == Channel::Reliable ? ENET_PACKET_FLAG_RELIABLE : 0;
        return enet_packet_create(bytes.data(), bytes.size(), flags);
    }
};

Transport::Transport() : m(std::make_unique<Impl>()) {}

Transport::~Transport()
{
    close();
    if (m->initialized) releaseEnet();
}

bool Transport::active() const
{
    return m->host != nullptr;
}

bool Transport::listen(uint16_t port, uint32_t maxConnections)
{
    close();
    if (!m->initialized && !(m->initialized = initEnet())) return false;
    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    address.port = port;
    m->host = enet_host_create(&address, maxConnections, kChannels, 0, 0);
    if (!m->host) {
        log::error("Network: can't listen on port {} (in use?)", port);
        return false;
    }
    return true;
}

bool Transport::connect(const std::string& addressName, uint16_t port)
{
    close();
    if (!m->initialized && !(m->initialized = initEnet())) return false;
    m->host = enet_host_create(nullptr, 1, kChannels, 0, 0);
    if (!m->host) return false;
    ENetAddress address{};
    if (enet_address_set_host(&address, addressName.c_str()) != 0) {
        log::error("Network: unknown host '{}'", addressName);
        close();
        return false;
    }
    address.port = port;
    ENetPeer* peer = enet_host_connect(m->host, &address, kChannels, 0);
    if (!peer) {
        close();
        return false;
    }
    const ConnectionId id = m->nextId++;
    peer->data = reinterpret_cast<void*>(uintptr_t(id));
    m->peers[id] = peer;
    return true;
}

void Transport::disconnect(ConnectionId c)
{
    auto it = m->peers.find(c);
    if (it == m->peers.end()) return;
    enet_peer_disconnect_later(it->second, 0);
}

void Transport::close()
{
    if (!m->host) return;
    // Polite goodbyes (a short wait for the acks), then drop whoever is left.
    for (auto& [id, p] : m->peers) enet_peer_disconnect(p, 0);
    ENetEvent e;
    for (int i = 0; i < 10 && !m->peers.empty() && enet_host_service(m->host, &e, 10) >= 0; ++i) {
        if (e.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(e.packet);
        if (e.type == ENET_EVENT_TYPE_DISCONNECT) m->peers.erase(m->idOf(e.peer));
    }
    for (auto& [id, p] : m->peers) enet_peer_reset(p);
    m->peers.clear();
    enet_host_destroy(m->host);
    m->host = nullptr;
}

void Transport::poll(const std::function<void(const Event&)>& onEvent)
{
    if (!m->host) return;
    ENetEvent e;
    while (m->host && enet_host_service(m->host, &e, 0) > 0) {
        switch (e.type) {
        case ENET_EVENT_TYPE_CONNECT: {
            ConnectionId id = m->idOf(e.peer);
            if (id == 0) { // an incoming connection (ours already have an id)
                id = m->nextId++;
                e.peer->data = reinterpret_cast<void*>(uintptr_t(id));
                m->peers[id] = e.peer;
            }
            enet_peer_timeout(e.peer, 0, 10000, 20000); // loading screens can stall a few seconds
            onEvent({Event::Type::Connected, id, {}});
            break;
        }
        case ENET_EVENT_TYPE_DISCONNECT: {
            const ConnectionId id = m->idOf(e.peer);
            m->peers.erase(id);
            e.peer->data = nullptr;
            onEvent({Event::Type::Disconnected, id, {}});
            break;
        }
        case ENET_EVENT_TYPE_RECEIVE:
            onEvent({Event::Type::Received, m->idOf(e.peer), std::span<const uint8_t>(e.packet->data, e.packet->dataLength)});
            enet_packet_destroy(e.packet);
            break;
        default: break;
        }
    }
}

void Transport::send(ConnectionId c, Channel channel, std::span<const uint8_t> bytes)
{
    auto it = m->peers.find(c);
    if (it == m->peers.end() || it->second->state != ENET_PEER_STATE_CONNECTED) return;
    enet_peer_send(it->second, uint8_t(channel), m->packet(channel, bytes));
}

void Transport::broadcast(Channel channel, std::span<const uint8_t> bytes, ConnectionId except)
{
    if (!m->host) return;
    ENetPacket* p = nullptr;
    for (auto& [id, peer] : m->peers) {
        if (id == except || peer->state != ENET_PEER_STATE_CONNECTED) continue;
        if (!p) p = m->packet(channel, bytes);
        enet_peer_send(peer, uint8_t(channel), p); // (ENet reference-counts the packet)
    }
}

void Transport::flush()
{
    if (m->host) enet_host_flush(m->host);
}

Transport::Stats Transport::stats(ConnectionId c) const
{
    Stats s;
    auto it = m->peers.find(c);
    if (it == m->peers.end()) return s;
    const ENetPeer* p = it->second;
    s.roundTripMs = p->roundTripTime;
    s.packetLoss = float(p->packetLoss) / float(ENET_PEER_PACKET_LOSS_SCALE);
    s.bytesSent = p->outgoingDataTotal;
    s.bytesReceived = p->incomingDataTotal;
    return s;
}

} // namespace ve::net
