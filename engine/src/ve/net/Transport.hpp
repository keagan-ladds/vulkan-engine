#pragma once
// =============================================================================
// Transport: UDP connections with reliable and unreliable channels (ENet,
// hidden behind this interface). Network (Network.hpp) is built on it; games
// normally don't use it directly.
//
//   Transport t;  t.listen(7777, 16);            // or t.connect("1.2.3.4", 7777)
//   t.poll([](const Transport::Event& e) { ... }); // every frame
//   t.send(peer, Channel::Reliable, bytes);
// =============================================================================

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ve::net {

using ConnectionId = uint32_t; ///< the transport's handle for one remote end (1..)

enum class Channel : uint8_t {
    Reliable = 0,   ///< ordered, resent until it arrives (spawns, RPCs, state)
    Unreliable = 1, ///< newest wins, may drop (movement)
};

/// Low-level UDP connections (ENet): listen / connect, reliable and unreliable
/// sends, events. Network builds sessions on it; games normally don't use it directly.
class Transport {
public:
    /// Something that happened on a connection (Transport::poll).
    struct Event {
        /// What happened: a connection opened, closed, or data arrived.
        enum class Type { Connected, Disconnected, Received };
        Type type = Type::Received;  ///< What happened.
        ConnectionId connection = 0;  ///< Which connection.
        std::span<const uint8_t> data; ///< Received: valid during the callback
    };
    /// Connection statistics.
    struct Stats {
        uint32_t roundTripMs = 0;  ///< Latency in milliseconds.
        uint64_t bytesSent = 0;      ///< Payload bytes sent on this connection since it opened.
        uint64_t bytesReceived = 0;  ///< Payload bytes received on this connection since it opened.
        uint64_t packetsSent = 0;     ///< Packets sent since it opened.
        uint64_t packetsReceived = 0; ///< Packets received since it opened.
        float packetLoss = 0.0f; ///< 0..1
    };
    /// Bad network conditions, for testing (everything this machine sends is
    /// affected; set it on both ends for a round trip). All zero = off.
    /// Reliable messages are only delayed (they still arrive, in order);
    /// unreliable ones can also be lost or arrive out of order.
    struct Simulation {
        uint32_t latencyMs = 0; ///< Added to every packet sent, one way (milliseconds).
        uint32_t jitterMs = 0;  ///< Plus a random 0..jitterMs (milliseconds).
        float loss = 0.0f;      ///< Chance (0..1) that an unreliable packet is dropped.
        /// Whether any of it is on.
        bool active() const { return latencyMs > 0 || jitterMs > 0 || loss > 0.0f; }
    };

    Transport();
    ~Transport();
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    bool listen(uint16_t port, uint32_t maxConnections);         ///< host / server
    bool connect(const std::string& address, uint16_t port);     ///< client: Connected arrives later (or Disconnected)
    void close();                                                ///< disconnects everyone, politely when possible
    /// Closes one connection (politely when possible).
    void disconnect(ConnectionId c);
    /// Whether it is listening or connected.
    bool active() const;

    void poll(const std::function<void(const Event&)>& onEvent); ///< receive; also keeps connections alive
    /// Queues `bytes` to one connection (sent on flush() or the next poll()).
    void send(ConnectionId c, Channel channel, std::span<const uint8_t> bytes);
    /// The same bytes to several connections, as one shared packet (not a copy per peer).
    void broadcast(std::span<const ConnectionId> to, Channel channel, std::span<const uint8_t> bytes);
    void flush(); ///< push queued packets out now (they also go on the next poll)
    /// Statistics of one connection.
    Stats stats(ConnectionId c) const;
    /// Turns the bad-network simulation on (or off with a default Simulation).
    void simulate(const Simulation& sim);
    /// The simulation in effect.
    const Simulation& simulation() const;
    /// Parses "latency=100,jitter=20,loss=0.05" (milliseconds, 0..1; any subset): VE_NET_SIM.
    static Simulation parseSimulation(const std::string& spec);

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace ve::net
