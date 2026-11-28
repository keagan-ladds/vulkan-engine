#include "Multiplayer.hpp"

#include "Building.hpp"
#include "Player.hpp"
#include "SavannaLayout.hpp"

#include <ve/anim/Animation.hpp>
#include <ve/assets/AssetManager.hpp>
#include <ve/core/Engine.hpp>
#include <ve/core/Env.hpp>
#include <ve/core/MathUtil.hpp>
#include <ve/core/Log.hpp>
#include <ve/net/NetworkAnimator.hpp>
#include <ve/physics/Physics.hpp>
#include <ve/render/Renderer.hpp>
#include <ve/scene/Components.hpp>
#include <ve/scene/Prefab.hpp>
#include <ve/scene/Reflection.hpp>
#include <ve/scene/SaveGame.hpp>
#include <ve/scene/Scene.hpp>
#include <ve/world/Foliage.hpp>
#include <ve/world/Terrain.hpp>
#include <ve/world/Vegetation.hpp>

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

namespace {
// Who may join: only Savanna, same protocol version (set before hosting or
// joining, from SAVANNA_NET or the menus).
void configure(net::Network& n)
{
    n.config.game = "savanna";
    n.config.version = 1;
}
} // namespace

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
        ve::GameObject& body = scene().instantiate(*m, this);
        body.transform().setLocalRotation(glm::angleAxis(3.14159265f, glm::vec3(0, 1, 0))); // the model faces +Z
        ve::Animator* animator = body.getComponent<ve::Animator>();
        m_animator = animator;
        if (animator) animator->play("Idle", 0.0f);
    } catch (const std::exception& e) {
        ve::log::warn("PlayerAvatar: {}", e.what());
    }
}

void PlayerAvatar::lateUpdate(float dt)
{
    auto* no = getComponent<net::NetworkObject>();
    if (no && no->isOwner()) {
        // Ours: follow the local player (NetworkTransform sends it at the end of the frame).
        if (const Player* player = localPlayer(scene())) {
            transform().setPosition(player->transform().position());
            const glm::vec3 f = player->camera() ? player->camera()->transform().forward() : glm::vec3(0, 0, -1);
            if (glm::length(glm::vec2(f.x, f.z)) > 1e-3f) transform().setRotation(ve::math::yawRotation(ve::math::yawOf(f)));
        }
        return;
    }
    // Someone else's: walk when it moves.
    const glm::vec3 p = transform().position();
    if (dt > 0.0f) m_speed = glm::mix(m_speed, glm::length(glm::vec2(p.x - m_last.x, p.z - m_last.z)) / dt, glm::min(1.0f, dt * 8.0f));
    m_last = p;
    const bool walking = m_speed > 0.4f;
    if (ve::Animator* animator = m_animator.get(scene()); animator && walking != m_walking) animator->play(walking ? "Walk" : "Idle", 0.3f);
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
    configure(n);
    if (const auto name = ve::env::get("SAVANNA_NAME")) n.playerName = *name;
    if (n.online()) return true;
    const auto spec = ve::env::get("SAVANNA_NET");
    if (!spec) return false;
    const std::string s(*spec);
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
    configure(n); // (menus host / join too, not just SAVANNA_NET)
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
    }, net::From::Host);
}

void Multiplayer::start()
{
    if (net().role() == net::Role::Host) spawnAvatar(net::kServer); // ours, for the others
    // Chopped trees and picked bushes are world state: the host's changes go
    // to everyone (as save state: the engine sends what changed).
    if (auto* f = scene().findFirst<ve::Foliage>()) {
        auto* no = f->getComponent<net::NetworkObject>();
        if (!no) no = &f->addComponent<net::NetworkObject>();
        no->replicateSaveState = true;
    }
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

namespace {
const char* yes(bool b)
{
    return b ? "yes" : "NO";
}
} // namespace

NetTest::NetTest(bool host) : GameObject("NetTest"), m_host(host)
{
    net::Network& n = net();
    if (host) {
        n.config.historySeconds = 10.0f; // (software rendering here: round trips of seconds)
        // A request that can be refused: negative numbers are.
        n.on("nettest.ask", [&n](net::PeerId, net::Reader& r) {
            if (r.read<int32_t>() < 0) n.reject("negative");
        }, net::From::Clients);
        // Only the host may send this one: a client asking must not run it here.
        n.on("nettest.hostonly", [this](net::PeerId, net::Reader&) { m_hostOnlyRan = true; }, net::From::Host);
        n.on("nettest.flood", [this](net::PeerId, net::Reader&) { ++m_floods; }, net::From::Clients);
        n.on("nettest.client-done", [this](net::PeerId, net::Reader&) { m_clientDone = true; }, net::From::Clients);
        return;
    }
    n.on("nettest.done", [this](net::PeerId, net::Reader&) { m_hostDone = true; }, net::From::Host);
    n.on("nettest.check", [this](net::PeerId, net::Reader& r) {
        m_crate = r.read<uint64_t>();
        m_crateAt = r.read<glm::vec3>();
        m_dancer = r.read<uint64_t>();
        m_foliage = r.read<uint64_t>();
        m_clip = r.readString();
        m_checkArrived = !r.failed();
    }, net::From::Host);
    n.on("nettest.far", [this](net::PeerId, net::Reader& r) {
        m_far = r.read<glm::vec3>();
        m_farArrived = !r.failed();
    }, net::From::Host);
    n.on("nettest.near", [this](net::PeerId, net::Reader& r) {
        m_near = r.read<glm::vec3>();
        m_nearArrived = !r.failed();
    }, net::From::Host);
    n.onPeersChanged = [this] { ++m_peerChanges; };
    auto previous = n.onDisconnected;
    n.onDisconnected = [this, previous](const std::string& reason) {
        m_disconnect = reason;
        if (previous) previous(reason);
    };
}

void NetTest::onDestroy()
{
    net::Network& n = net();
    for (const char* name : {"nettest.ask", "nettest.hostonly", "nettest.flood", "nettest.client-done", "nettest.done", "nettest.check",
                             "nettest.far", "nettest.near"})
        n.off(name);
    if (!m_host) n.onPeersChanged = nullptr;
}

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
    if (m_total > 900.0f && m_step < 100) {
        log("TIMEOUT at step " + std::to_string(m_step));
        m_step = 100;
        engine().quit();
        return;
    }
    if (m_host) hostStep(dt);
    else clientStep(dt);
}

void NetTest::hostStep(float)
{
    net::Network& n = net();
    auto* mp = scene().findFirst<Multiplayer>();
    Player* player = localPlayer(scene());
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
    const net::PeerId client = n.peers().size() > 1 ? n.peers()[1].id : net::kEveryone;
    PlayerAvatar* theirs = mp && client != net::kEveryone ? mp->avatarOf(client) : nullptr;

    switch (m_step) {
    case 0: // before anyone joins: a piece late joiners must get from the snapshot
        if (m_t < 1.0f) break;
        requestPiece("prefabs/building/foundation_stone.prefab", site);
        log("hosting: " + std::string(yes(n.role() == net::Role::Host)) + ", placed piece A");
        next();
        break;
    case 1: {
        if (n.peers().size() < 2 || !theirs) break;
        const net::Network::PeerInfo& p = n.peers()[1];
        log("a player joined: " + p.name + " (peer " + std::to_string(p.id) + "), avatars " + std::to_string(mp ? mp->avatarCount() : -1));
        // Identity: a stable player id, and data kept per player id.
        const bool idOk = p.playerId.size() >= 8 && p.playerId != n.localPlayerId() && n.findPlayer(p.playerId) == p.id;
        const int visits = ve::save::readPlayer("nettest", p.playerId).value_or(ve::Json::object()).value("visits", 0) + 1;
        ve::save::writePlayer("nettest", p.playerId, {{"visits", visits}});
        const int readBack = ve::save::readPlayer("nettest", p.playerId).value_or(ve::Json::object()).value("visits", 0);
        n.setPlayerObject(p.id, theirs);
        log(std::string("player id '") + p.playerId + "': " + yes(idOk) + ", their save data kept per player id: " + yes(readBack == visits) +
            " (visit " + std::to_string(visits) + "), player object " + yes(n.playerObject(p.id) == theirs));
        next();
        break;
    }
    case 2: { // walk in a circle where the client looks; place piece B halfway
        const float a = m_t * 0.8f;
        const glm::vec2 c = site + glm::vec2(0.0f, -6.0f);
        if (player) player->setPose(ground(c + 3.0f * glm::vec2(std::cos(a), std::sin(a))), glm::degrees(-a), 0.0f);
        if (m_t > 4.0f && pieces() == 1) {
            requestPiece("prefabs/building/foundation_stone.prefab", site + glm::vec2(2.0f, 0.0f));
            log("placed piece B while they watch");
        }
        // Lag compensation: our avatar where the client saw it (a moment ago).
        PlayerAvatar* ours = mp ? mp->avatarOf(net::kServer) : nullptr;
        if (m_t > 6.0f && !m_rewound && ours && client != net::kEveryone) {
            m_rewound = true;
            const glm::vec3 now = ours->transform().position();
            glm::vec3 then = now;
            n.rewind(client, [&] { then = ours->transform().position(); });
            const double ago = n.clock() - n.viewTime(client);
            auto* nt = ours->getComponent<net::NetworkTransform>();
            glm::vec3 p1(0.0f), p2(0.0f);
            glm::quat q;
            const bool h1 = nt && nt->poseAt(n.clock() - 0.3, p1, q), h2 = nt && nt->poseAt(n.clock() - 100.0, p2, q);
            log("lag compensation history: 0.3 s ago " + std::string(yes(h1)) + " " + std::to_string(glm::distance(p1, now)) + " m away, oldest " +
                yes(h2) + " " + std::to_string(glm::distance(p2, now)) + " m away");
            log("lag compensation: rewound our avatar " + std::to_string(glm::distance(now, then)) + " m back to " +
                std::to_string(int(ago * 1000.0)) + " ms ago; moved back after: " + yes(glm::distance(ours->transform().position(), now) < 1e-4f) +
                "; " + yes(glm::distance(now, then) > 0.01f && ago > 0.09));
        }
        if (m_t > 12.0f) next();
        break;
    }
    case 3: // the client asks for piece C
        if (pieces() < 3 && m_t < 90.0f) break;
        log("the client's piece C arrived: " + std::string(yes(pieces() >= 3)) + " (pieces " + std::to_string(pieces()) + ")");
        next();
        break;
    case 4: { // the client's avatar should be where the client stands
        if (theirs && player) { // look at them
            const glm::vec3 p = theirs->transform().position();
            const glm::vec3 eye = p + glm::vec3(-4.0f, 0.5f, 5.0f);
            const glm::vec3 d = p + glm::vec3(0, 1.0f, 0) - (eye + glm::vec3(0, 1.7f, 0));
            player->setPose(eye, glm::degrees(ve::math::yawOf(d)), glm::degrees(ve::math::pitchOf(d)));
        }
        if (!theirs || m_t < 2.5f) break;
        if (!m_shot) {
            m_shot = true;
            if (const auto dir = ve::env::get("SAVANNA_SHOTS")) renderer().requestCapture(std::string(*dir) + "/net-host.png");
            break;
        }
        const glm::vec3 p = theirs->transform().position();
        log("their avatar: '" + theirs->playerName + "' at " + std::to_string(p.x) + ", " + std::to_string(p.z));
        // Things for them to watch: a crate that falls (dynamic here, follows
        // there), a villager whose Animator is replicated, a tree we remove.
        if (ve::GameObject* crate = n.spawn("prefabs/net/test_crate.prefab", p + glm::vec3(3.0f, 4.0f, 0.0f))) m_crate = crate->id();
        if (ve::GameObject* dancer = n.spawn("prefabs/net/test_dancer.prefab", ground(glm::vec2(p.x - 3.0f, p.z + 2.0f)))) m_dancer = dancer->id();
        if (auto* f = scene().findFirst<ve::Foliage>(); f && f->system()) {
            for (uint32_t layer = 0; layer < f->layerNames().size() && !m_foliage; ++layer)
                if (auto inst = f->system()->nearest(layer, glm::vec2(p.x, p.z), 80.0f)) {
                    f->system()->setState(inst->id, ve::kFoliageRemoved);
                    m_foliage = inst->id.packed();
                }
        }
        log(std::string("spawned a crate ") + yes(m_crate != 0) + ", a dancer " + yes(m_dancer != 0) + ", removed a tree " + yes(m_foliage != 0));
        next();
        break;
    }
    case 5: { // the dancer waves (on our machine: the client sees it through NetworkAnimator)
        if (m_t < 3.0f) break;
        auto* dancer = scene().findById(m_dancer);
        auto* na = dancer ? dancer->getComponent<net::NetworkAnimator>() : nullptr;
        if (ve::Animator* a = na ? na->animator() : nullptr) a->play("Wave", 0.2f);
        next();
        break;
    }
    case 6: { // the crate has come to rest: tell them where, and what to look for
        if (m_t < 6.0f) break;
        ve::GameObject* crate = scene().findById(m_crate);
        net::Writer w;
        w.write(m_crate);
        w.write(crate ? crate->transform().position() : glm::vec3(0.0f));
        w.write(m_dancer);
        w.write(m_foliage);
        w.writeString("Wave");
        n.toClients("nettest.check", w);
        next();
        break;
    }
    case 7: { // interest management: the crate goes far away (out of their range: not sent)...
        if (m_t < 8.0f) break;
        ve::GameObject* crate = scene().findById(m_crate);
        glm::vec3 view{0.0f};
        n.viewpoint(client, view);
        const glm::vec3 far = ground(glm::vec2(view.x + 300.0f, view.z)) + glm::vec3(0, 1.0f, 0);
        if (auto* rb = crate ? crate->getComponent<ve::Rigidbody>() : nullptr) rb->teleport(far, glm::quat(1, 0, 0, 0));
        net::Writer w;
        w.write(far);
        n.toClients("nettest.far", w);
        next();
        break;
    }
    case 8: { // ...and comes back near them (in range again: they catch up)
        if (m_t < 6.0f) break;
        ve::GameObject* crate = scene().findById(m_crate);
        glm::vec3 view{0.0f};
        n.viewpoint(client, view);
        m_near = ground(glm::vec2(view.x - 4.0f, view.z - 3.0f)) + glm::vec3(0, 0.6f, 0);
        if (auto* rb = crate ? crate->getComponent<ve::Rigidbody>() : nullptr) rb->teleport(m_near, glm::quat(1, 0, 0, 0));
        net::Writer w;
        w.write(m_near);
        n.toClients("nettest.near", w);
        next();
        break;
    }
    case 9: { // they flood us with requests, then say they're done
        if (!m_clientDone && m_t < 120.0f) break;
        const net::Network::Bandwidth& b = n.bandwidth();
        const net::Network::PeerInfo* p = n.peer(client);
        log("client done: " + std::string(yes(m_clientDone)) + "; request budget: " + std::to_string(m_floods) + " of 300 floods handled, " +
            std::to_string(b.droppedRequests) + " dropped " + yes(b.droppedRequests > 0) + "; a client-sent host-only message ran here: " +
            (m_hostOnlyRan ? "YES (wrong)" : "no (right)"));
        log("bandwidth: " + std::to_string(int(b.sentPerSecond)) + " B/s out, " + std::to_string(b.sent / 1024) + " KB sent in all; to them " +
            std::to_string(int(p ? p->sentPerSecond : 0.0f)) + " B/s: " + yes(b.sent > 0));
        n.toClients("nettest.done"); // they can go
        next();
        break;
    }
    case 10: // ...and we send them off
        if (m_t < 2.0f) break;
        if (client != net::kEveryone) n.kick(client, "test over");
        next();
        break;
    case 11: // they've gone: their avatar too
        if (n.peers().size() > 1 && m_t < 90.0f) break;
        log("the player left: " + std::string(yes(n.peers().size() == 1)) + ", avatars left " + std::to_string(mp ? mp->avatarCount() : -1) +
            " (just ours)");
        engine().quit();
        next();
        break;
    default: break;
    }
}

void NetTest::clientStep(float)
{
    net::Network& n = net();
    auto* mp = scene().findFirst<Multiplayer>();
    Player* player = localPlayer(scene());
    auto next = [&] {
        ++m_step;
        m_t = 0.0f;
    };
    const glm::vec2 site = layout::kCamp + glm::vec2(4.0f, 14.0f);
    const ve::Terrain* terrain = ve::Terrain::find(scene());
    auto ground = [&](glm::vec2 p) { return glm::vec3(p.x, terrain ? terrain->height(p.x, p.y) : 3.0f, p.y); };

    switch (m_step) {
    case 0:
        if (n.status() == net::Status::Connected) {
            log("joined as peer " + std::to_string(n.localPeer()) + ", " + std::to_string(n.peers().size()) + " players, our player id " +
                n.localPlayerId());
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
        log("from the snapshot: piece A " + std::string(yes(pieces() >= 1)) + ", the host's avatar " + yes(host) + ", our own avatar " +
            yes(mp && mp->avatarOf(n.localPeer())) + "; player list updates " + std::to_string(m_peerChanges) + " " + yes(m_peerChanges > 0));
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
                player->setPose(eye, glm::degrees(ve::math::yawOf(d)), glm::degrees(ve::math::pitchOf(d)));
            }
        }
        if (pieces() >= 2 && m_moved > 3.0f && !m_shot && m_t > 6.0f) {
            m_shot = true;
            if (const auto dir = ve::env::get("SAVANNA_SHOTS")) renderer().requestCapture(std::string(*dir) + "/net-client.png");
        }
        if ((pieces() < 2 || m_moved < 3.0f || m_t < 7.0f) && m_t < 60.0f) break;
        log("the host's avatar walked " + std::to_string(m_moved) + " m (interpolated), piece B arrived: " + yes(pieces() >= 2) +
            ", host name '" + (host ? host->playerName : std::string("?")) + "', our player object " +
            yes(n.playerObject(n.localPeer()) == (mp ? mp->avatarOf(n.localPeer()) : nullptr)));
        // Now ask for one: building goes through the host.
        net::Writer w;
        w.writeString("prefabs/building/foundation_stone.prefab");
        w.write(ground(site + glm::vec2(-2.0f, 0.0f)));
        w.writeRotation(glm::quat(1, 0, 0, 0));
        n.toServer("build.place", w);
        next();
        break;
    }
    case 3: { // C comes back; requests with answers; a message only the host may send
        if (pieces() < 3 && m_t < 60.0f) break;
        log("our requested piece C came back from the host: " + std::string(yes(pieces() >= 3)) + ", " +
            std::to_string(n.stats().roundTripMs) + " ms round trip");
        net::Writer ok, bad;
        ok.write(int32_t(5));
        bad.write(int32_t(-5));
        n.toServer("nettest.ask", ok, [this](bool accepted, const std::string&) {
            m_accepted = accepted;
            ++m_replies;
        });
        n.toServer("nettest.ask", bad, [this](bool accepted, const std::string& reason) {
            m_refused = !accepted;
            m_refusal = reason;
            ++m_replies;
        });
        n.toServer("nettest.hostonly");
        next();
        break;
    }
    case 4: // stand still (the host looks at our avatar); the replies come back
        if (m_replies < 2 && m_t < 30.0f) break;
        log("requests: accepted " + std::string(yes(m_accepted)) + ", refused " + yes(m_refused) + " ('" + m_refusal + "')");
        next();
        break;
    case 5: { // what the host made: crate, dancer, a removed tree
        if (!m_checkArrived) break;
        if (m_t < 3.0f) break; // (a moment for the crate's last moves to arrive)
        ve::GameObject* crate = scene().findById(m_crate);
        auto* rb = crate ? crate->getComponent<ve::Rigidbody>() : nullptr;
        const float off = crate ? glm::distance(crate->transform().position(), m_crateAt) : -1.0f;
        log("crate: here " + std::string(yes(crate)) + ", its body follows the host (kinematic here) " +
            yes(rb && rb->type == ve::BodyType::Kinematic) + ", " + std::to_string(off) + " m from where it landed there " +
            yes(off >= 0.0f && off < 0.05f));
        ve::GameObject* dancer = scene().findById(m_dancer);
        auto* na = dancer ? dancer->getComponent<net::NetworkAnimator>() : nullptr;
        ve::Animator* a = na ? na->animator() : nullptr;
        log("dancer: animator " + std::string(yes(a)) + ", playing '" + (a ? a->current(0) : std::string()) + "' like the host " +
            yes(a && a->current(0) == m_clip));
        auto* f = scene().findFirst<ve::Foliage>();
        const uint32_t state = f && f->system() ? f->system()->state(ve::FoliageInstanceId::unpack(m_foliage)) : 0;
        log("the tree the host removed is gone here too: " + std::string(yes(m_foliage && state == ve::kFoliageRemoved)));
        next();
        break;
    }
    case 6: { // the host moved the crate far from us: we don't hear about it
        if (!m_farArrived) break;
        if (m_t < 3.0f) break;
        ve::GameObject* crate = scene().findById(m_crate);
        const float d = crate ? glm::distance(glm::vec2(crate->transform().position().x, crate->transform().position().z), glm::vec2(m_far.x, m_far.z)) : 0.0f;
        log("interest: the crate's move out of our range wasn't sent: " + std::string(yes(d > 100.0f)) + " (" + std::to_string(int(d)) +
            " m from where it went)");
        next();
        break;
    }
    case 7: { // ...back near us: we catch up
        if (!m_nearArrived) break;
        ve::GameObject* crate = scene().findById(m_crate);
        const glm::vec3 p = crate ? crate->transform().position() : glm::vec3(0.0f);
        const float d = glm::distance(glm::vec2(p.x, p.z), glm::vec2(m_near.x, m_near.z));
        if (d > 0.3f && m_t < 20.0f) break;
        log("interest: back in range, it caught up: " + std::string(yes(d <= 0.3f)) + " (" + std::to_string(d) + " m)");
        // The request budget: far more than the host takes.
        for (int i = 0; i < 300; ++i) n.toServer("nettest.flood");
        next();
        break;
    }
    case 8: // (a pause for the budget to refill) then tell the host we're done
        if (m_t < 3.0f) break;
        n.toServer("nettest.client-done");
        next();
        break;
    case 9: // the host kicks us when it's done
        if (m_disconnect.empty() && m_t < 120.0f) break;
        log("the host sent us off: " + std::string(yes(m_hostDone && m_disconnect.rfind("Kicked", 0) == 0)) + " ('" + m_disconnect + "')");
        engine().quit();
        next();
        break;
    default: break;
    }
}

} // namespace savanna
