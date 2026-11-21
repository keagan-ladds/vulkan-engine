#include "Pickups.hpp"

#include "Inventory.hpp"
#include "Player.hpp"
#include "Props.hpp"
#include "SavannaLayout.hpp"

#include <ve/assets/AssetManager.hpp>
#include <ve/audio/Audio.hpp>
#include <ve/core/Engine.hpp>
#include <ve/gameplay/Interaction.hpp>
#include <ve/gameplay/ProxySet.hpp>
#include <ve/render/Mesh.hpp>
#include <ve/render/Renderer.hpp>
#include <ve/scene/Scene.hpp>
#include <ve/world/Terrain.hpp>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

namespace savanna {

namespace {

constexpr const char* kSetName = "Pickups";
constexpr float kSpacing = 9.0f;      // one candidate per 9 x 9 m (a jittered grid)
constexpr float kKeep = 0.85f;        // chance a dry spot gets something
constexpr float kStickShare = 0.4f;   // the rest are stones
constexpr float kClearOfSunstones = 6.0f; // (the sunstones' "Pick up" must stay theirs)

// Deterministic per-cell randomness: a small integer hash (splitmix64), so the
// scatter and hence the entry ids are the same with every compiler.
uint64_t mix(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
float rand01(int gx, int gz, uint32_t salt)
{
    const uint64_t k = mix((uint64_t(uint32_t(gx)) << 32) ^ uint64_t(uint32_t(gz)) ^ (uint64_t(salt) << 48) ^ 0x5A17A11Aull);
    return float(k >> 40) / float(1u << 24);
}

// How each item looks lying in the grass: its inventory mesh, scaled down.
struct Kind {
    const char* item;
    float scale, scaleJitter;
};
constexpr Kind kStone{"stone", 0.24f, 0.08f};
constexpr Kind kStick{"stick", 0.5f, 0.15f};

ve::ProxyType pickupType(const Kind& kind)
{
    ve::ProxyType t;
    t.name = kind.item;
    t.lods = {{ve::Mesh::create(ve::Engine::get().renderer(), itemMesh(kind.item), {}, std::string("pickup ") + kind.item), {propMaterial()}}};
    t.cullDistance = 80.0f; // small: not worth drawing further
    t.shadowDistance = 25.0f;
    t.promoteDistance = 6.0f; // the Interactor reaches 3 m: a margin for walking up
    const std::string item = kind.item;
    t.promote = [item](ve::Scene& s, const ve::ProxyEntry&) -> ve::GameObject* { return &s.spawn<Pickup>(item); };
    return t;
}

} // namespace

ve::ProxySet* findPickups(const ve::Scene& scene)
{
    return dynamic_cast<ve::ProxySet*>(scene.find(kSetName));
}

ve::ProxySet& spawnPickups(ve::Scene& scene, const ve::Terrain& terrain, const Player& player)
{
    auto& set = scene.spawn<ve::ProxySet>(kSetName);
    set.focus = &player;
    const uint32_t stone = set.addType(pickupType(kStone));
    const uint32_t stick = set.addType(pickupType(kStick));

    std::vector<glm::vec2> sunstones;
    for (ve::GameObject* o : scene.findAllWithTag("collectible"))
        sunstones.push_back({o->transform().position().x, o->transform().position().z});
    auto place = [&](glm::vec2 p, int gx, int gz) {
        const float h = terrain.height(p.x, p.y);
        if (h < terrain.waterLevel + 0.3f) return; // not in the swamp's water
        for (glm::vec2 s : sunstones)
            if (glm::distance(s, p) < kClearOfSunstones) return;
        if (glm::distance(p, layout::kCamp) < 3.0f) return; // the campfire
        const bool isStick = rand01(gx, gz, 3) < kStickShare;
        const Kind& kind = isStick ? kStick : kStone;
        const float yaw = rand01(gx, gz, 4) * glm::two_pi<float>();
        const float scale = kind.scale + (rand01(gx, gz, 5) * 2.0f - 1.0f) * kind.scaleJitter;
        set.add(isStick ? stick : stone, {p.x, h - 0.02f, p.y}, glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec3(scale));
    };

    // A handful around the camp, where you start (and where the screenshots look).
    const glm::vec2 start = layout::kCamp + glm::vec2(9.0f, 5.0f);
    for (int i = 0; i < 10; ++i) {
        const float a = float(i) * 2.39996f; // golden angle: evenly spread
        const float r = 3.5f + 5.0f * rand01(i, -1, 1);
        place(start + r * glm::vec2(std::cos(a), std::sin(a)), i, -1000);
    }
    // ...and the rest everywhere: a jittered grid over the map.
    const float half = layout::kHalfSize - 20.0f;
    const int n = int(2.0f * half / kSpacing);
    for (int gz = 0; gz < n; ++gz) {
        for (int gx = 0; gx < n; ++gx) {
            if (rand01(gx, gz, 0) > kKeep) continue;
            const glm::vec2 p(-half + (float(gx) + rand01(gx, gz, 1)) * kSpacing, -half + (float(gz) + rand01(gx, gz, 2)) * kSpacing);
            place(p, gx, gz);
        }
    }
    return set;
}

// -----------------------------------------------------------------------------
// Pickup
// -----------------------------------------------------------------------------

Pickup::Pickup(std::string item) : GameObject(item == "stick" ? "Stick" : "Stone"), m_item(std::move(item))
{
    tag = "pickup";
}

void Pickup::start()
{
    // No collider: the player's Interactor finds it in its view cone.
    auto& use = addComponent<ve::Interactable>();
    use.prompt = "Pick up " + m_item; // "Pick up stone"
    use.focusOffset = {0.0f, 0.3f, 0.0f};
    use.onUse = [this](ve::GameObject&) { pickUp(); };
}

void Pickup::pickUp()
{
    if (isDestroyed()) return;
    gameState().inventory.add(m_item, 1);
    const ItemType* type = itemType(m_item);
    notify("+1 " + (type ? type->name : m_item));
    if (auto sound = assets().loadAudio("audio/pickup.wav")) audio().playOneShot(*sound, 0.4f);
    // Gone for good: the set notices its object went and drops the entry
    // (ve::ProxyLink::consume() would do it at once).
    destroy();
}

} // namespace savanna
