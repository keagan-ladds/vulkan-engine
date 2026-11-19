#include "Multiplayer.hpp"

#include "Building.hpp"
#include "Player.hpp"
#include "SavannaLayout.hpp"

#include <ve/anim/Animation.hpp>
#include <ve/assets/AssetManager.hpp>
#include <ve/core/Engine.hpp>
#include <ve/core/Log.hpp>
#include <ve/render/Renderer.hpp>
#include <ve/scene/Components.hpp>
#include <ve/scene/Prefab.hpp>
#include <ve/scene/Reflection.hpp>
#include <ve/scene/SaveGame.hpp>
#include <ve/scene/Scene.hpp>
#include <ve/world/Terrain.hpp>

#include <vfx/DayNight.hpp>
#include <vfx/Weather.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace savanna {

VE_REGISTER_TYPE(PlayerAvatar, "PlayerAvatar")
{
    t.category("Savanna");
    t.field("playerName", &PlayerAvatar::playerName);
}

namespace net = ve::net;

// -----------------------------------------------------------------------------
// PlayerAvatar
// -----------------------------------------------------------------------------

void PlayerAvatar::start()
{
    m_last = transform().position();
    if (auto* no = getComponent<net::NetworkObject>(); no && no->isOwner()) {
        playerName = net().playerName; // ours: tell the others who we are
        no->markDirty();
        return; // (no body: we see the world through our own eyes)
    }
    try {
        auto m = assets().loadModel("models/villager.gltf");
        m_body = &scene().instantiate(*m, this);
        m_body->transform().setLocalRotation(glm::angleAxis(3.14159265f, glm::vec3(0, 1, 0))); // the model faces +Z
        m_animator = m_body->getComponent<ve::Animator>();
        if (m_animator) m_animator->play("Idle", 0.0f);
    } catch (const std::exception& e) {
        ve::log::warn("PlayerAvatar: {}", e.what());
    }
}

void PlayerAvatar::lateUpdate(float dt)
{
    auto* no = getComponent<net::NetworkObject>();
    if (no && no->isOwner()) {
        // Ours: follow the local player (NetworkTransform sends it at the end of the frame).
        if (auto* player = dynamic_cast<Player*>(scene().find("Player"))) {
            transform().setPosition(player->transform().position());
            glm::vec3 f = player->camera() ? player->camera()->transform().forward() : glm::vec3(0, 0, -1);
            f.y = 0.0f;
            if (glm::length(f) > 1e-3f) {
                f = glm::normalize(f);
                transform().setRotation(glm::angleAxis(std::atan2(-f.x, -f.z), glm::vec3(0, 1, 0)));
            }
        }
        return;
    }
    // Someone else's: walk when it moves.
    const glm::vec3 p = transform().position();
    if (dt > 0.0f) m_speed = glm::mix(m_speed, glm::length(glm::vec2(p.x - m_last.x, p.z - m_last.z)) / dt, glm::min(1.0f, dt * 8.0f));
    m_last = p;
    const bool walking = m_speed > 0.4f;
    if (m_animator && walking != m_walking) m_animator->play(walking ? "Walk" : "Idle", 0.3f);
    m_walking = walking;
    if (!m_named && !playerName.empty()) {
        m_named = true;
        setName("Avatar: " + playerName);
    }
}

// -----------------------------------------------------------------------------
// Multiplayer
// -----------------------------------------------------------------------------

bool Multiplayer::startFromEnvironment(ve::Engine& engine)
{
    net::Network& n = engine.net();
    n.config.game = "savanna";
    n.config.version = 1;
    if (const char* name = std::getenv("SAVANNA_NAME")) n.playerName = name;
    if (n.online()) return true;
    const char* spec = std::getenv("SAVANNA_NET");
    if (!spec) return false;
    const std::string s = spec;
    auto portAfter = [](const std::string& str, size_t colon) {
        return colon == std::string::npos ? net::kDefaultPort : uint16_t(std::atoi(str.c_str() + colon + 1));
    };
    if (s.rfind("host", 0) == 0) {
        if (n.playerName == "Player") n.playerName = "Host";
        return n.host(portAfter(s, s.find(':')));
    }
    if (s.rfind("join:", 0) == 0) {
        const std::string rest = s.substr(5);
        const size_t colon = rest.find(':');
        return n.join(rest.substr(0, colon), portAfter(rest, colon));
    }
    ve::log::warn("SAVANNA_NET: use host[:port] or join:<address>[:port]");
    return false;
}

Multiplayer::Multiplayer() : GameObject("Multiplayer")
{
    runInEditor = false;
    // (Hooked up now, not in start(): messages that waited for the loading
    // screen are handled at the top of the next frame, before start() runs.)
    net::Network& n = net();
    n.config.game = "savanna"; // (menus host / join too, not just SAVANNA_NET)
    n.config.version = 1;
    // Host: what joining players get (the save capture minus our own player:
    // they keep theirs), and an avatar for each.
    n.worldSnapshot = [this](net::PeerId) {
        ve::Json world = ve::save::capture(scene());
        if (world.contains("state"))
            for (auto it = world["state"].begin(); it != world["state"].end();) {
                if (it.key().rfind("name:Player", 0) == 0) it = world["state"].erase(it);
                else ++it;
            }
        return world;
    };
    n.onPeerJoined = [this](net::PeerId peer) { spawnAvatar(peer); };
    // Client: the host's world over the level we just built.
    n.onJoined = [this](const ve::Json& world) { ve::save::apply(scene(), world); };
    n.onDisconnected = [](const std::string& reason) { ve::log::warn("Multiplayer: {}", reason); };
    // The host's sky for everyone.
    n.on("world.clock", [this](net::PeerId, net::Reader& r) {
        const float time = r.read<float>();
        const auto weather = vfx::Weather::Type(r.read<uint8_t>());
        if (r.failed() || net().isServer()) return;
        if (auto* dn = scene().findFirst<vfx::DayNightCycle>(); dn && std::abs(dn->timeOfDay - time) > 0.05f) dn->timeOfDay = time;
        if (auto* w = scene().findFirst<vfx::Weather>(); w && w->weather() != weather && weather < vfx::Weather::Type::Count) w->setWeather(weather, 8.0f);
    });
}

void Multiplayer::start()
{
    if (net().role() == net::Role::Host) spawnAvatar(net::kServer); // ours, for the others
}

void Multiplayer::onDestroy()
{
    net::Network& n = net();
    n.off("world.clock");
    n.worldSnapshot = nullptr;
    n.onPeerJoined = nullptr;
    n.onJoined = nullptr;
    n.onDisconnected = nullptr;
}

void Multiplayer::spawnAvatar(net::PeerId owner)
{
    // Everyone starts at the camp (the clients' own players do too).
    glm::vec2 at = layout::kCamp + glm::vec2(9.0f + 1.5f * float(owner), 5.0f);
    const ve::Terrain* t = ve::Terrain::find(scene());
    if (ve::GameObject* o = net().spawn(kAvatarPrefab, {at.x, t ? t->height(at.x, at.y) : 3.0f, at.y}, glm::quat(1, 0, 0, 0), owner))
        if (auto* pi = o->getComponent<ve::PrefabInstance>()) pi->saveInGame = false; // players aren't part of the world's save
}

void Multiplayer::update(float dt)
{
    // The host's clock and weather, every couple of seconds.
    net::Network& n = net();
    if (n.role() != net::Role::Host || n.peers().size() < 2) return;
    m_clockTimer -= dt;
    if (m_clockTimer > 0.0f) return;
    m_clockTimer = 2.0f;
    net::Writer w;
    const auto* dn = scene().findFirst<vfx::DayNightCycle>();
    const auto* weather = scene().findFirst<vfx::Weather>();
    w.write(dn ? dn->timeOfDay : 12.0f);
    w.write(uint8_t(weather ? weather->weather() : vfx::Weather::Type::Clear));
    n.toClients("world.clock", w);
}

PlayerAvatar* Multiplayer::avatarOf(net::PeerId peer) const
{
    PlayerAvatar* found = nullptr;
    for (const auto& o : scene().objects())
        if (auto* a = dynamic_cast<PlayerAvatar*>(o.get()); a && !a->isDestroyed())
            if (auto* no = a->getComponent<net::NetworkObject>(); no && no->owner() == peer) found = a;
    return found;
}

int Multiplayer::avatarCount() const
{
    int n = 0;
    for (const auto& o : scene().objects())
        if (dynamic_cast<PlayerAvatar*>(o.get()) && !o->isDestroyed()) ++n;
    return n;
}

// -----------------------------------------------------------------------------
// NetTest: host and client processes on one machine (tools/nettest.sh)
// -----------------------------------------------------------------------------

void NetTest::log(const std::string& s) const
{
    std::fprintf(stderr, "[nettest %s] %s\n", m_host ? "host" : "client", s.c_str());
}

int NetTest::pieces() const
{
    int n = 0;
    for (const auto& o : scene().objects())
        if (dynamic_cast<BuildPiece*>(o.get()) && !o->isDestroyed() && o->getComponent<ve::PrefabInstance>()) ++n;
    return n;
}

void NetTest::update(float)
{
    // Real time: the other process runs at its own pace (and the network does).
    const float dt = time().unscaledDeltaTime;
    m_t += dt;
    m_total += dt;
    net::Network& n = net();
    auto* mp = scene().findFirst<Multiplayer>();
    auto* player = dynamic_cast<Player*>(scene().find("Player"));
    auto next = [&] {
        ++m_step;
        m_t = 0.0f;
    };
    const glm::vec2 site = layout::kCamp + glm::vec2(4.0f, 14.0f);
    const ve::Terrain* terrain = ve::Terrain::find(scene());
    auto ground = [&](glm::vec2 p) { return glm::vec3(p.x, terrain ? terrain->height(p.x, p.y) : 3.0f, p.y); };
    auto requestPiece = [&](const char* prefab, glm::vec2 at) {
        net::Writer w;
        w.writeString(prefab);
        w.write(ground(at));
        w.writeRotation(glm::quat(1, 0, 0, 0));
        n.toServer("build.place", w);
    };
    if (m_total > 300.0f && m_step < 100) {
        log("TIMEOUT at step " + std::to_string(m_step));
        m_step = 100;
        engine().quit();
        return;
    }

    if (m_host) {
        switch (m_step) {
        case 0: // before anyone joins: a piece late joiners must get from the snapshot
            if (m_t < 1.0f) break;
            requestPiece("prefabs/building/foundation_stone.prefab", site);
            log("hosting: " + std::string(n.role() == net::Role::Host ? "yes" : "NO") + ", placed piece A");
            next();
            break;
        case 1:
            if (n.peers().size() < 2) break;
            log("a player joined: " + n.peers()[1].name + " (peer " + std::to_string(n.peers()[1].id) + "), avatars " +
                std::to_string(mp ? mp->avatarCount() : -1));
            next();
            break;
        case 2: { // walk in a circle where the client looks; place piece B halfway
            const float a = m_t * 0.8f;
            const glm::vec2 c = site + glm::vec2(0.0f, -6.0f);
            if (player) player->setPose(ground(c + 3.0f * glm::vec2(std::cos(a), std::sin(a))), glm::degrees(-a), 0.0f);
            if (m_t > 4.0f && pieces() == 1) {
                requestPiece("prefabs/building/foundation_stone.prefab", site + glm::vec2(2.0f, 0.0f));
                log("placed piece B while they watch");
            }
            if (m_t > 12.0f) next();
            break;
        }
        case 3: // the client asks for piece C
            if (pieces() < 3 && m_t < 60.0f) break;
            log("the client's piece C arrived: " + std::string(pieces() >= 3 ? "yes" : "NO") + " (pieces " + std::to_string(pieces()) + ")");
            next();
            break;
        case 4: { // the client's avatar should be where the client stands
            PlayerAvatar* theirs = mp && n.peers().size() > 1 ? mp->avatarOf(n.peers()[1].id) : nullptr;
            if (theirs && player) { // look at them
                const glm::vec3 p = theirs->transform().position();
                const glm::vec3 eye = p + glm::vec3(-4.0f, 0.5f, 5.0f);
                const glm::vec3 d = p + glm::vec3(0, 1.0f, 0) - (eye + glm::vec3(0, 1.7f, 0));
                player->setPose(eye, glm::degrees(std::atan2(-d.x, -d.z)), glm::degrees(std::atan2(d.y, glm::length(glm::vec2(d.x, d.z)))));
            }
            if (!theirs || m_t < 2.5f) break;
            if (!m_shot) {
                m_shot = true;
                if (const char* dir = std::getenv("SAVANNA_SHOTS")) renderer().requestCapture(std::string(dir) + "/net-host.png");
                break;
            }
            log("their avatar: '" + theirs->playerName + "' at " + std::to_string(theirs->transform().position().x) + ", " +
                std::to_string(theirs->transform().position().z));
            n.toClients("nettest.done"); // they can go
            next();
            break;
        }
        case 5: // they leave: their avatar goes
            if (n.peers().size() > 1 && m_t < 90.0f) break;
            log("the player left: " + std::string(n.peers().size() == 1 ? "yes" : "NO") + ", avatars left " +
                std::to_string(mp ? mp->avatarCount() : -1) + " (just ours)");
            engine().quit();
            next();
            break;
        default: break;
        }
        return;
    }

    switch (m_step) {
    case 0:
        if (n.status() == net::Status::Connected) {
            log("joined as peer " + std::to_string(n.localPeer()) + ", " + std::to_string(n.peers().size()) + " players");
            next();
        } else if (n.status() == net::Status::Failed) {
            log("FAILED to join: " + n.lastError());
            engine().quit();
            m_step = 100;
        }
        break;
    case 1: { // the snapshot and the spawns have arrived
        PlayerAvatar* host = mp ? mp->avatarOf(net::kServer) : nullptr;
        if ((!host || pieces() < 1) && m_t < 30.0f) break;
        log("from the snapshot: piece A " + std::string(pieces() >= 1 ? "yes" : "NO") + ", the host's avatar " + (host ? "yes" : "NO") +
            ", our own avatar " + (mp && mp->avatarOf(n.localPeer()) ? "yes" : "NO"));
        if (host) m_seen = host->transform().position();
        next();
        break;
    }
    case 2: { // watch the host walk; look at it
        PlayerAvatar* host = mp ? mp->avatarOf(net::kServer) : nullptr;
        if (host) {
            const glm::vec3 p = host->transform().position();
            m_moved += glm::distance(p, m_seen);
            m_seen = p;
            if (player) {
                const glm::vec3 eye = p + glm::vec3(5.0f, 1.0f, 6.0f);
                const glm::vec3 d = p + glm::vec3(0, 1.0f, 0) - (eye + glm::vec3(0, 1.7f, 0));
                player->setPose(eye, glm::degrees(std::atan2(-d.x, -d.z)), glm::degrees(std::atan2(d.y, glm::length(glm::vec2(d.x, d.z)))));
            }
        }
        if (pieces() >= 2 && m_moved > 3.0f && !m_shot && m_t > 6.0f) {
            m_shot = true;
            if (const char* dir = std::getenv("SAVANNA_SHOTS")) renderer().requestCapture(std::string(dir) + "/net-client.png");
        }
        if ((pieces() < 2 || m_moved < 3.0f || m_t < 7.0f) && m_t < 40.0f) break;
        log("the host's avatar walked " + std::to_string(m_moved) + " m (interpolated), piece B arrived: " + (pieces() >= 2 ? "yes" : "NO") +
            ", host name '" + (host ? host->playerName : std::string("?")) + "'");
        // Now ask for one: building goes through the host.
        requestPiece("prefabs/building/foundation_stone.prefab", site + glm::vec2(-2.0f, 0.0f));
        next();
        break;
    }
    case 3:
        if (pieces() < 3 && m_t < 30.0f) break;
        log("our requested piece C came back from the host: " + std::string(pieces() >= 3 ? "yes" : "NO") + ", " +
            std::to_string(n.stats().roundTripMs) + " ms round trip");
        next();
        break;
    case 4: // stand still (the host looks at our avatar) until it's done, then leave
        if (!m_hostDone && m_t < 120.0f) break;
        log(std::string("leaving (the host was done: ") + (m_hostDone ? "yes" : "NO") + ")");
        n.leave();
        engine().quit();
        next();
        break;
    default: break;
    }
}

} // namespace savanna
