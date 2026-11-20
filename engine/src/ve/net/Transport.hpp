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

using ConnectionId = uint32_t; // the transport's handle for one remote end (1..)

enum class Channel : uint8_t {
    Reliable = 0,   // ordered, resent until it arrives (spawns, RPCs, state)
    Unreliable = 1, // newest wins, may drop (movement)
};

class Transport {
public:
    struct Event {
        enum class Type { Connected, Disconnected, Received } type;
        ConnectionId connection = 0;
        std::span<const uint8_t> data; // Received: valid during the callback
    };
    struct Stats {
        uint32_t roundTripMs = 0;
        uint64_t bytesSent = 0, bytesReceived = 0;
        float packetLoss = 0.0f; // 0..1
    };

    Transport();
    ~Transport();
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    bool listen(uint16_t port, uint32_t maxConnections);         // host / server
    bool connect(const std::string& address, uint16_t port);     // client: Connected arrives later (or Disconnected)
    void close();                                                // disconnects everyone, politely when possible
    void disconnect(ConnectionId c);
    bool active() const;

    void poll(const std::function<void(const Event&)>& onEvent); // receive; also keeps connections alive
    void send(ConnectionId c, Channel channel, std::span<const uint8_t> bytes);
    // The same bytes to several connections, as one shared packet (not a copy per peer).
    void broadcast(std::span<const ConnectionId> to, Channel channel, std::span<const uint8_t> bytes);
    void flush(); // push queued packets out now (they also go on the next poll)
    Stats stats(ConnectionId c) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace ve::net
