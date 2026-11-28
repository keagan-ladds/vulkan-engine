#include "ve/net/Transport.hpp"

#include "ve/core/Log.hpp"

#include <enet/enet.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <random>
#include <unordered_map>
#include <vector>

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

double nowSeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

struct Transport::Impl {
    /// A packet held back by the simulation.
    struct Delayed {
        double due;
        ConnectionId connection;
        Channel channel;
        std::vector<uint8_t> bytes;
    };
    /// Our own counters (ENet resets its byte counts every second).
    struct Counters {
        uint64_t bytesSent = 0, bytesReceived = 0, packetsSent = 0, packetsReceived = 0;
    };
    ENetHost* host = nullptr;
    bool initialized = false;
    ConnectionId nextId = 1;
    std::unordered_map<ConnectionId, ENetPeer*> peers;
    std::unordered_map<ConnectionId, Counters> counters;
    Simulation sim;
    std::deque<Delayed> delayed; ///< sorted by due
    std::unordered_map<ConnectionId, double> lastReliableDue; ///< reliable packets keep their order
    std::mt19937 rng{12345};

    void sendNow(ConnectionId c, Channel channel, std::span<const uint8_t> bytes)
    {
        auto it = peers.find(c);
        if (it == peers.end() || it->second->state != ENET_PEER_STATE_CONNECTED) return;
        ENetPacket* p = packet(channel, bytes);
        // On failure ENet doesn't take the packet: it's still ours to free.
        if (enet_peer_send(it->second, uint8_t(channel), p) != 0) {
            enet_packet_destroy(p);
            log::warn("Network: couldn't queue a {}-byte packet for connection {}", bytes.size(), c);
            return;
        }
        Counters& k = counters[c];
        k.bytesSent += bytes.size();
        ++k.packetsSent;
    }
    // Through the simulation: dropped, or held until its time.
    void sendSimulated(ConnectionId c, Channel channel, std::span<const uint8_t> bytes)
    {
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        if (channel == Channel::Unreliable && unit(rng) < sim.loss) return;
        double due = nowSeconds() + (double(sim.latencyMs) + double(sim.jitterMs) * unit(rng)) / 1000.0;
        if (channel == Channel::Reliable) {
            double& last = lastReliableDue[c];
            due = std::max(due, last);
            last = due;
        }
        Delayed d{due, c, channel, {bytes.begin(), bytes.end()}};
        auto at = std::upper_bound(delayed.begin(), delayed.end(), due, [](double t, const Delayed& e) { return t < e.due; });
        delayed.insert(at, std::move(d));
    }
    void releaseDue()
    {
        const double t = nowSeconds();
        while (!delayed.empty() && delayed.front().due <= t) {
            Delayed d = std::move(delayed.front());
            delayed.pop_front();
            sendNow(d.connection, d.channel, d.bytes);
        }
    }

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
    m->counters.clear();
    m->delayed.clear();
    m->lastReliableDue.clear();
    enet_host_destroy(m->host);
    m->host = nullptr;
}

void Transport::poll(const std::function<void(const Event&)>& onEvent)
{
    if (!m->host) return;
    m->releaseDue();
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
            m->counters.erase(id);
            m->lastReliableDue.erase(id);
            e.peer->data = nullptr;
            onEvent({Event::Type::Disconnected, id, {}});
            break;
        }
        case ENET_EVENT_TYPE_RECEIVE: {
            Impl::Counters& k = m->counters[m->idOf(e.peer)];
            k.bytesReceived += e.packet->dataLength;
            ++k.packetsReceived;
            onEvent({Event::Type::Received, m->idOf(e.peer), std::span<const uint8_t>(e.packet->data, e.packet->dataLength)});
            enet_packet_destroy(e.packet);
            break;
        }
        default: break;
        }
    }
}

void Transport::send(ConnectionId c, Channel channel, std::span<const uint8_t> bytes)
{
    if (m->sim.active()) m->sendSimulated(c, channel, bytes);
    else m->sendNow(c, channel, bytes);
}

void Transport::broadcast(std::span<const ConnectionId> to, Channel channel, std::span<const uint8_t> bytes)
{
    if (!m->host) return;
    if (m->sim.active()) {
        for (const ConnectionId c : to) m->sendSimulated(c, channel, bytes);
        return;
    }
    ENetPacket* p = nullptr;
    for (const ConnectionId c : to) {
        auto it = m->peers.find(c);
        if (it == m->peers.end() || it->second->state != ENET_PEER_STATE_CONNECTED) continue;
        if (!p) p = m->packet(channel, bytes);
        // (ENet reference-counts the packet: one copy of the bytes for everyone.)
        if (enet_peer_send(it->second, uint8_t(channel), p) != 0) {
            log::warn("Network: couldn't queue a {}-byte packet for connection {}", bytes.size(), c);
            continue;
        }
        Impl::Counters& k = m->counters[c];
        k.bytesSent += bytes.size();
        ++k.packetsSent;
    }
    // Nobody took it (every send failed): it's still ours to free.
    if (p && p->referenceCount == 0) enet_packet_destroy(p);
}

void Transport::flush()
{
    if (!m->host) return;
    m->releaseDue();
    enet_host_flush(m->host);
}

void Transport::simulate(const Simulation& sim)
{
    m->sim = sim;
    if (sim.active()) log::info("Network: simulating {} ms (+0..{}) latency, {:.0f}% loss", sim.latencyMs, sim.jitterMs, sim.loss * 100.0f);
}

const Transport::Simulation& Transport::simulation() const
{
    return m->sim;
}

Transport::Simulation Transport::parseSimulation(const std::string& spec)
{
    Simulation sim;
    size_t start = 0;
    while (start < spec.size()) {
        size_t end = spec.find(',', start);
        if (end == std::string::npos) end = spec.size();
        const std::string item = spec.substr(start, end - start);
        const size_t eq = item.find('=');
        if (eq != std::string::npos) {
            const std::string key = item.substr(0, eq);
            const double v = std::atof(item.c_str() + eq + 1);
            if (key == "latency") sim.latencyMs = uint32_t(std::max(0.0, v));
            else if (key == "jitter") sim.jitterMs = uint32_t(std::max(0.0, v));
            else if (key == "loss") sim.loss = float(std::clamp(v, 0.0, 1.0));
            else log::warn("VE_NET_SIM: unknown setting '{}' (latency, jitter, loss)", key);
        }
        start = end + 1;
    }
    return sim;
}

Transport::Stats Transport::stats(ConnectionId c) const
{
    Stats s;
    auto it = m->peers.find(c);
    if (it == m->peers.end()) return s;
    const ENetPeer* p = it->second;
    s.roundTripMs = p->roundTripTime;
    s.packetLoss = float(p->packetLoss) / float(ENET_PEER_PACKET_LOSS_SCALE);
    if (auto k = m->counters.find(c); k != m->counters.end()) {
        s.bytesSent = k->second.bytesSent;
        s.bytesReceived = k->second.bytesReceived;
        s.packetsSent = k->second.packetsSent;
        s.packetsReceived = k->second.packetsReceived;
    }
    return s;
}

} // namespace ve::net
