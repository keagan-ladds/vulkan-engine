#include "Props.hpp"

#include "Harvest.hpp"

#include <ve/world/Noise.hpp>
#include "Player.hpp"
#include "SavannaLayout.hpp"

#include <ve/world/Terrain.hpp>
#include "Vegetation.hpp"
#include "Villager.hpp"
#include "World.hpp"

#include <ve/anim/Animation.hpp>
#include <ve/assets/AssetManager.hpp>
#include <ve/scene/SaveGame.hpp>
#include <ve/gameplay/Interaction.hpp>
#include <ve/nav/Navigation.hpp>
#include <ve/audio/Ambience.hpp>
#include <ve/audio/Audio.hpp>
#include <ve/audio/Music.hpp>
#include <ve/core/Engine.hpp>
#include <ve/editor/Editor.hpp>
#include <ve/core/Input.hpp>
#include <ve/core/Log.hpp>
#include <ve/core/Time.hpp>
#include <ve/physics/Physics.hpp>
#include <ve/render/Instancing.hpp>
#include <ve/render/Mesh.hpp>
#include <ve/render/Primitives.hpp>
#include <ve/render/Renderer.hpp>
#include <ve/world/Foliage.hpp>
#include <ve/world/Vegetation.hpp>
#include <ve/scene/Components.hpp>
#include <ve/scene/Scene.hpp>
#include <ve/render/Material.hpp>
#include <ve/scene/Reflection.hpp>
#include <vfx/PhotoMode.hpp>
#include <vfx/Fire.hpp>
#include <vfx/Particles.hpp>
#include <vfx/DayNight.hpp>
#include <vfx/Water.hpp>
#include <vfx/Weather.hpp>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <cstdio>

namespace savanna {

namespace {
void forSubtree(ve::Transform& t, const std::function<void(ve::Transform&)>& fn)
{
    fn(t);
    for (ve::Transform* c : t.children()) forSubtree(*c, fn);
}
} // namespace


GameState& gameState()
{
    static GameState state;
    return state;
}

namespace {

// Flicker: sum of a few sines at unrelated frequencies + noise.
float flicker(float t, float seed)
{
    return 0.75f + 0.12f * std::sin(t * 9.1f + seed) + 0.08f * std::sin(t * 23.7f + seed * 3.0f) +
           0.1f * (ve::noise::valueNoise(glm::vec2(t * 6.0f, seed)) - 0.5f);
}


} // namespace

// -----------------------------------------------------------------------------
// Campfire
// -----------------------------------------------------------------------------

namespace {
// Weak: the objects using the material keep it alive, nothing outlives the renderer.
std::weak_ptr<ve::Material>& propMaterialSlot()
{
    static std::weak_ptr<ve::Material> material;
    return material;
}
} // namespace

void setPropMaterial(std::shared_ptr<ve::Material> material) { propMaterialSlot() = material; }

std::shared_ptr<ve::Material> propMaterial()
{
    if (auto m = propMaterialSlot().lock()) return m;
    auto mat = ve::Engine::get().renderer().createStandardMaterial();
    mat->setBaseColor(glm::vec4(1.0f)).setRoughness(0.85f).setVertexColorWeight(1.0f);
    propMaterialSlot() = mat;
    return mat;
}

// --- Reflection: these types can be created in the editor and stored in scene files.
VE_REGISTER_TYPE(Campfire, "Campfire")
{
    t.category("Savanna");
    t.field("baseIntensity", &Campfire::baseIntensity).range(0.0f, 100.0f).speed(0.2f);
}
VE_REGISTER_TYPE(Torch, "Torch")
{
    t.category("Savanna");
    t.field("intensity", &Torch::intensity).range(0.0f, 50.0f).speed(0.1f);
}
VE_REGISTER_TYPE(Sunstone, "Sunstone") { t.category("Savanna"); }
VE_REGISTER_TYPE(ReedScreen, "ReedScreen") { t.category("Savanna"); }
VE_REGISTER_TYPE(PlayerStart, "PlayerStart") { t.category("Savanna"); }

void Campfire::start()
{
    m_material = propMaterial();
    ve::Renderer& r = renderer();
    ve::Scene& s = scene();

    // Stone ring + crossed logs, built from primitives.
    ve::MeshData md;
    for (int i = 0; i < 10; ++i) {
        const float a = static_cast<float>(i) / 10.0f * glm::two_pi<float>();
        glm::mat4 t = glm::translate(glm::mat4(1.0f), glm::vec3(std::cos(a) * 0.9f, 0.1f, std::sin(a) * 0.9f));
        t = glm::scale(t, glm::vec3(0.28f, 0.2f, 0.24f));
        md.append(ve::primitives::icosphere(1.0f, 1), t, {0.38f, 0.36f, 0.33f, 1.0f});
    }
    for (int i = 0; i < 4; ++i) {
        const float a = static_cast<float>(i) / 4.0f * glm::pi<float>();
        glm::mat4 t = glm::translate(glm::mat4(1.0f), glm::vec3(std::cos(a) * 0.55f, 0.1f, std::sin(a) * 0.55f));
        t = glm::rotate(t, -a, glm::vec3(0, 1, 0));
        t = glm::rotate(t, glm::radians(70.0f), glm::vec3(0, 0, 1));
        md.append(ve::primitives::cylinder(0.08f, 0.07f, 1.0f, 6), t, {0.22f, 0.14f, 0.08f, 1.0f});
    }
    md.makeFlatShaded();
    addComponent<ve::MeshRenderer>(ve::Mesh::create(r, md, {}, "campfire"), m_material);

    // Light: the fire's glow, with cube-map shadows.
    auto& lightObj = s.spawn("FireLight");
    lightObj.setParent(this, false);
    lightObj.transform().setLocalPosition({0.0f, 0.9f, 0.0f});
    m_light = &lightObj.addComponent<ve::PointLight>();
    m_light->color = {1.0f, 0.52f, 0.2f};
    m_light->range = 16.0f;
    m_light->castShadows = true;
    m_light->shadowBias = 0.004f;

    // Flames (procedural, vfx::Fire) over a bed of glowing coals; smoke and sparks are particles.
    auto& flames = s.spawn("Flames");
    flames.setParent(this, false);
    flames.transform().setLocalPosition({0.0f, 0.1f, 0.0f});
    m_fire = &flames.addComponent<vfx::Fire>();
    m_fire->height = 1.4f;
    m_fire->radius = 0.5f;
    m_fire->intensity = 4.0f;
    m_fire->layers = 4;
    m_fire->seed = 3;
    if (const char* h = std::getenv("SAVANNA_HAZE")) m_fire->haze = float(std::atof(h)); // comparisons

    auto& smokeObj = s.spawn("Smoke");
    smokeObj.setParent(this, false);
    smokeObj.transform().setLocalPosition({0.0f, 1.2f, 0.0f});
    auto& smoke = smokeObj.addComponent<vfx::ParticleEmitter>().settings;
    smoke.rate = 6.0f;
    smoke.lifetimeMin = 4.0f;
    smoke.lifetimeMax = 7.0f;
    smoke.spawnRadius = 0.25f;
    smoke.velocity = {0.0f, 1.1f, 0.0f};
    smoke.velocityRandom = {0.2f, 0.2f, 0.2f};
    smoke.windInfluence = 0.6f;
    smoke.sizeStart = 0.4f;
    smoke.sizeEnd = 3.2f;
    smoke.colorStart = {0.25f, 0.23f, 0.22f, 0.0f};
    smoke.colorMid = {0.22f, 0.21f, 0.2f, 0.35f};
    smoke.colorEnd = {0.3f, 0.3f, 0.3f, 0.0f};
    smoke.midPoint = 0.15f;
    smoke.softness = 1.0f;
    smoke.texture = r.graph().findPass<vfx::ParticlePass>()->smokeSprite;

    auto& sparkObj = s.spawn("Sparks");
    sparkObj.setParent(this, false);
    sparkObj.transform().setLocalPosition({0.0f, 0.4f, 0.0f});
    auto& sparks = sparkObj.addComponent<vfx::ParticleEmitter>().settings;
    sparks.rate = 10.0f;
    sparks.lifetimeMin = 0.8f;
    sparks.lifetimeMax = 1.8f;
    sparks.spawnRadius = 0.3f;
    sparks.velocity = {0.0f, 3.0f, 0.0f};
    sparks.velocityRandom = {0.8f, 1.2f, 0.8f};
    sparks.gravity = {0.0f, -0.6f, 0.0f};
    sparks.windInfluence = 0.3f;
    sparks.sizeStart = sparks.sizeEnd = 0.025f;
    sparks.colorStart = {12.0f, 5.0f, 1.0f, 1.0f};
    sparks.colorMid = {8.0f, 2.5f, 0.4f, 1.0f};
    sparks.colorEnd = {2.0f, 0.3f, 0.0f, 0.0f};
    sparks.blend = ve::BlendMode::Additive;
    sparks.stretchWithVelocity = true;
    sparks.stretch = 0.05f;

    // Crackling fire, positioned in 3D: louder as you approach, panned left/right.
    auto& audioSrc = addComponent<ve::AudioSource>(assets().loadAudio("audio/fire_loop.wav"));
    audioSrc.loop = true;
    audioSrc.playOnStart = true;
    audioSrc.volume = 0.15f;
    audioSrc.minDistance = 1.5f;
    audioSrc.maxDistance = 28.0f;
    audioSrc.occlusion = true; // muffled behind huts or terrain
}

void Campfire::update(float dt)
{
    m_t += dt;
    if (!m_light) return;
    // The glow follows the flames: brightness and a small wobble (shadows dance).
    m_light->intensity = baseIntensity * (m_fire ? m_fire->flicker() : flicker(m_t, 1.3f));
    if (m_fire) m_light->transform().setLocalPosition(glm::vec3(0.0f, 0.9f, 0.0f) + m_fire->lightJitter());
}

// -----------------------------------------------------------------------------
// Torch
// -----------------------------------------------------------------------------

void Torch::start()
{
    m_material = propMaterial();
    ve::MeshData md;
    md.append(ve::primitives::cylinder(0.05f, 0.04f, 1.9f, 6), glm::mat4(1.0f), {0.25f, 0.16f, 0.09f, 1.0f});
    md.append(ve::primitives::cylinder(0.09f, 0.11f, 0.25f, 6), glm::translate(glm::mat4(1.0f), {0, 1.85f, 0}),
              {0.12f, 0.09f, 0.07f, 1.0f});
    md.makeFlatShaded();
    addComponent<ve::MeshRenderer>(ve::Mesh::create(renderer(), md, {}, "torch"), m_material);

    auto& flame = scene().spawn("TorchFlame");
    flame.setParent(this, false);
    flame.transform().setLocalPosition({0.0f, 1.97f, 0.0f});
    m_fire = &flame.addComponent<vfx::Fire>();
    m_fire->height = 0.5f;
    m_fire->radius = 0.12f;
    m_fire->intensity = 3.5f;
    m_fire->layers = 3;
    m_fire->coals = 0.0f;
    m_fire->speed = 1.3f;
    m_fire->haze = 0.6f;
    if (const char* h = std::getenv("SAVANNA_HAZE")) m_fire->haze *= float(std::atof(h));
    m_fire->seed = uint32_t(reinterpret_cast<uintptr_t>(this) % 997u);
    auto& glow = scene().spawn("TorchLight");
    glow.setParent(&flame, false);
    glow.transform().setLocalPosition({0.0f, 0.2f, 0.0f});
    // A thin solid pole (static capsule standing on the ground).
    addComponent<ve::Rigidbody>(ve::Collider::capsule(0.95f, 0.07f).offset({0.0f, 1.0f, 0.0f}), ve::BodyType::Static);
    m_light = &glow.addComponent<ve::PointLight>();
    m_light->color = {1.0f, 0.55f, 0.22f};
    m_light->range = 10.0f;
    m_light->castShadows = true;
    m_light->shadowBias = 0.006f;
    m_t = static_cast<float>(reinterpret_cast<uintptr_t>(this) % 1000);
}

void Torch::update(float dt)
{
    m_t += dt;
    if (!m_light) return;
    m_light->intensity = intensity * (m_fire ? m_fire->flicker() : flicker(m_t, 5.7f));
    if (m_fire) m_light->transform().setLocalPosition(glm::vec3(0.0f, 0.2f, 0.0f) + m_fire->lightJitter());
}

// -----------------------------------------------------------------------------
// Sunstone
// -----------------------------------------------------------------------------

int Sunstone::remaining(const ve::Scene& scene)
{
    int n = 0;
    for (const auto& o : scene.objects()) n += (!o->isDestroyed() && dynamic_cast<const Sunstone*>(o.get())) ? 1 : 0;
    return n;
}

void Sunstone::start()
{
    // Shared by all stones (created once).
    static std::weak_ptr<ve::Mesh> sharedMesh;
    static std::weak_ptr<ve::Material> sharedMaterial;
    auto mesh = sharedMesh.lock();
    if (!mesh) {
        ve::MeshData data = ve::primitives::icosphere(0.22f, 0);
        data.makeFlatShaded();
        mesh = ve::Mesh::create(renderer(), data, {}, "sunstone");
        sharedMesh = mesh;
    }
    auto material = sharedMaterial.lock();
    if (!material) {
        auto m = renderer().createStandardMaterial();
        m->setBaseColor({1.0f, 0.6f, 0.25f, 1.0f}).setRoughness(0.3f).setEmissive({1.0f, 0.45f, 0.12f}, 6.0f);
        material = m;
        sharedMaterial = material;
    }
    m_sound = assets().loadAudio("audio/pickup.wav");
    m_player = dynamic_cast<Player*>(scene().find("Player"));

    m_base = transform().position();
    // No collider: the player's Interactor finds it in its view cone.
    auto& use = addComponent<ve::Interactable>();
    use.prompt = "Pick up sunstone";
    use.focusOffset = {0.0f, 0.6f, 0.0f};
    use.onUse = [this](ve::GameObject&) { collect(); };
    m_visual = &scene().spawn("SunstoneVisual");
    m_visual->setParent(this, false);
    m_visual->addComponent<ve::MeshRenderer>(mesh, material);
    auto& glow = m_visual->addComponent<ve::PointLight>();
    glow.color = {1.0f, 0.65f, 0.25f};
    glow.intensity = 2.5f;
    glow.range = 4.0f;
    m_t = m_base.x * 0.37f;
}

void Sunstone::update(float dt)
{
    m_t += dt;
    m_visual->transform().setLocalPosition({0.0f, 0.6f + 0.15f * std::sin(m_t * 2.0f), 0.0f});
    m_visual->transform().setLocalRotation(glm::angleAxis(m_t * 1.2f, glm::normalize(glm::vec3(0.3f, 1.0f, 0.1f))));

}

void Sunstone::collect()
{
    if (isDestroyed()) return;
    GameState& gs = gameState();
    ++gs.collected;
    gs.total = gs.collected + remaining(scene()) - 1; // this one is still in the scene until the frame ends
    gs.message = "Sunstone collected! (" + std::to_string(gs.collected) + "/" + std::to_string(gs.total) + ")";
    gs.messageTimer = 2.5f;
    if (m_sound) audio().playOneShot(*m_sound, 0.8f);
    destroy(); // deferred to the end of the frame; children (visual + light) go too
}

// -----------------------------------------------------------------------------
// ReedScreen
// -----------------------------------------------------------------------------

void ReedScreen::start()
{
    static std::weak_ptr<ve::Mesh> sharedMesh;
    static std::weak_ptr<ve::Material> sharedMaterial;
    auto mesh = sharedMesh.lock();
    if (!mesh) {
        mesh = ve::Mesh::create(renderer(), ve::primitives::quad(2.6f, 1.9f), {}, "reed screen");
        sharedMesh = mesh;
    }
    auto material = sharedMaterial.lock();
    if (!material) {
        auto m = renderer().createStandardMaterial(ve::StandardMaterial::Mode::Cutout);
        m->setAlbedoTexture(assets().loadTexture("textures/reed_screen.png"))
            .setUvScale({3.0f, 2.0f})
            .setAlphaCutoff(0.5f)
            .setRoughness(0.9f)
            .setVertexColorWeight(0.0f);
        material = m;
        sharedMaterial = material;
    }
    addComponent<ve::MeshRenderer>(mesh, material);
    addComponent<ve::Rigidbody>(ve::Collider::box({1.3f, 0.95f, 0.03f}).offset({0.0f, 0.95f, 0.0f}), ve::BodyType::Static);
}

// -----------------------------------------------------------------------------
// AutoTest
// -----------------------------------------------------------------------------

namespace {

// Counts physics events on its object (autotest).
class EventProbe : public ve::Component {
public:
    int enters = 0, stays = 0, exits = 0, triggerEnters = 0, triggerExits = 0;
    float maxImpact = 0.0f;
    std::string lastTrigger;
    void onCollisionEnter(const ve::Collision& c) override
    {
        ++enters;
        maxImpact = std::max(maxImpact, c.impactSpeed);
    }
    void onCollisionStay(const ve::Collision&) override { ++stays; }
    void onCollisionExit(const ve::Collision&) override { ++exits; }
    void onTriggerEnter(ve::GameObject& other) override
    {
        ++triggerEnters;
        lastTrigger += (lastTrigger.empty() ? "" : ", ") + other.name();
    }
    void onTriggerExit(ve::GameObject&) override { ++triggerExits; }
};

// A PointLight subclass added under its own type: Scene::forEach<PointLight>
// (which the renderer uses) must still find it.
class ProbeLight : public ve::PointLight {};

// Spawns another object from onDestroy() (Scene::flushDestroyed must cope).
class SpawnOnDestroy : public ve::GameObject {
public:
    SpawnOnDestroy() : GameObject("SpawnOnDestroy") {}
    void onDestroy() override { scene().spawn("SpawnedFromOnDestroy"); }
};

} // namespace

namespace {
std::string stateLabel(const ve::FoliageSystem& f, ve::FoliageInstanceId id)
{
    const std::string& n = f.stateName(id.layer, f.state(id));
    return n.empty() ? std::string("(default)") : n;
}
} // namespace

void AutoTest::shot(const char* name)
{
    // SAVANNA_SHOTS=<folder>: screenshots of the interactive foliage steps (headless runs).
    if (const char* dir = std::getenv("SAVANNA_SHOTS"); dir && *dir && !m_shotTaken.count(name)) {
        m_shotTaken.insert(name);
        renderer().requestCapture(std::string(dir) + "/" + name + ".png");
    }
}

glm::vec3 AutoTest::groundAt(glm::vec2 p) const
{
    return {p.x, m_terrain->height(p.x, p.y) + 0.1f, p.y};
}

void AutoTest::update(float dt)
{
    m_t += dt;
    auto log = [](const std::string& s) { std::fprintf(stderr, "[autotest] %s\n", s.c_str()); };
    // Music mood and ambient layer levels (target levels: the fades take a few seconds).
    auto logAudio = [&](const std::string& where) {
        std::string layers;
        scene().forEach<ve::AmbientLayer>([&](ve::AmbientLayer& l) {
            char b[96];
            std::snprintf(b, sizeof(b), "%s%s %.2f", layers.empty() ? "" : ", ", l.gameObject().name().c_str(), l.targetLevel());
            layers += b;
        });
        log("audio @ " + where + ": music '" + audio().music().currentMood() + "' (" + audio().music().currentTrack() +
            "), Ambient bus x" + std::to_string(audio().bus("Ambient").zoneVolume) + ", Music bus x" +
            std::to_string(audio().bus("Music").zoneVolume) + "; layers: " + layers);
    };
    if (m_step == 0 && m_t > 3.0f) {
        auto stones = scene().findAllWithTag("collectible");
        log("sunstones in scene: " + std::to_string(stones.size()));
        if (!stones.empty()) static_cast<Sunstone*>(stones.front())->collect();
        ++m_step;
        // Scene registry: subclass components, and spawning from onDestroy().
        {
            ve::GameObject& probe = scene().spawn("ProbeLight");
            probe.addComponent<ProbeLight>().intensity = 0.0f;
            int seen = 0;
            scene().forEach<ve::PointLight>([&](ve::PointLight& l) { seen += &l.gameObject() == &probe; });
            log("PointLight subclass seen by forEach<PointLight>: " + std::string(seen == 1 ? "yes" : "NO"));
            probe.destroy();
            m_destroyProbe = &scene().spawn<SpawnOnDestroy>();
        }
        // Physics: ray straight down onto the terrain, player standing, crates resting.
        const glm::vec3 above(layout::kCamp.x + 3.0f, 200.0f, layout::kCamp.y + 3.0f);
        if (auto hit = physics().raycast(above, {0, -1, 0}, 400.0f)) {
            log("raycast down hit '" + (hit->gameObject ? hit->gameObject->name() : std::string("?")) +
                "' at y=" + std::to_string(hit->point.y) + " (terrain says " +
                std::to_string(m_terrain->height(above.x, above.z)) + ")");
        } else {
            log("raycast down MISSED");
        }
        if (auto* p = static_cast<Player*>(scene().find("Player"))) {
            log(std::string("player grounded: ") + (p->grounded() ? "yes" : "no") + ", feet y=" +
                std::to_string(p->transform().position().y));
            const auto look = physics().raycast(p->eyePosition(), p->lookDirection(), 25.0f);
            log("eye raycast: " + (look ? (look->gameObject ? look->gameObject->name() : std::string("<no object>")) +
                                               " at " + std::to_string(look->distance) + " m"
                                        : std::string("nothing")));
            p->throwRock();
        }
        // Physics queries, and a trigger box the player is teleported into.
        {
            const glm::vec2 tp = layout::kCamp + glm::vec2(-4.0f, 4.0f); // clear ground between the fire and the reed screens
            const glm::vec3 center(tp.x, m_terrain->height(tp.x, tp.y) + 1.0f, tp.y);
            auto& trigger = scene().spawn("TestTrigger");
            trigger.transform().setPosition(center);
            auto& rb = trigger.addComponent<ve::Rigidbody>(ve::Collider::box({1.5f, 1.5f, 1.5f}), ve::BodyType::Static);
            rb.isTrigger = true;
            trigger.addComponent<EventProbe>();
            if (auto* p = scene().find("Player")) {
                p->addComponent<EventProbe>();
                m_triggerFeet = center - glm::vec3(0.0f, 1.0f, 0.0f);
            }
            const auto down = physics().sphereCast(above, 0.5f, {0, -1, 0}, 400.0f);
            log("sphereCast down: " + (down ? (down->gameObject ? down->gameObject->name() : std::string("?")) + " at y=" +
                                                  std::to_string(down->point.y) + ", normal.y " + std::to_string(down->normal.y)
                                           : std::string("MISSED")));
            auto crateList = scene().findAllWithTag("crate");
            if (!crateList.empty()) {
                const glm::vec3 c = crateList.front()->transform().position();
                int crateHits = 0;
                for (const auto& h : physics().overlapSphere(c, 2.0f))
                    if (h.gameObject && h.gameObject->tag == "crate") ++crateHits;
                const auto box = physics().boxCast(c + glm::vec3(0.0f, 0.0f, 6.0f), glm::vec3(0.2f), glm::quat(1, 0, 0, 0),
                                                   {0, 0, -1}, 10.0f);
                log("overlapSphere(2 m) at a crate: " + std::to_string(crateHits) + " crates; boxCast towards it hit " +
                    (box ? (box->gameObject ? box->gameObject->name() : std::string("?")) + " at " +
                               std::to_string(box->distance) + " m"
                         : std::string("nothing")));
            }
            m_triggerCenter = center;
        }
        auto crates = scene().findAllWithTag("crate");
        float minY = 1e9f, maxY = -1e9f;
        for (auto* c : crates) {
            minY = std::min(minY, c->transform().position().y);
            maxY = std::max(maxY, c->transform().position().y);
        }
        log("crates: " + std::to_string(crates.size()) + ", y range " + std::to_string(minY) + " .. " + std::to_string(maxY));
        logAudio("start (camp)");
    } else if (m_step == 1 && !m_inTrigger && m_t > 3.3f) {
        // The thrown rock is away: stand in the trigger box for a moment.
        if (auto* p = static_cast<Player*>(scene().find("Player"))) p->teleport(m_triggerFeet, 0.0f);
        ve::QueryFilter f;
        f.triggers = true;
        f.statics = f.dynamics = false;
        const auto inside = physics().overlapSphere(m_triggerCenter, 0.5f, f);
        const auto ray = physics().raycast(m_triggerCenter + glm::vec3(0.0f, 5.0f, 0.0f), {0, -1, 0}, 10.0f);
        log("trigger queries: overlap(triggers only) found " + std::to_string(inside.size()) +
            (inside.empty() || !inside[0].gameObject ? std::string() : " (" + inside[0].gameObject->name() + ")") +
            ", default raycast through it hit " + (ray && ray->gameObject ? ray->gameObject->name() : std::string("nothing")));
        m_inTrigger = true;
    } else if (m_step == 1 && m_t > 4.0f) {
        if (m_destroyProbe) m_destroyProbe->destroy(); // checked at step 2
        m_destroyProbe = nullptr;
        if (auto* probe = scene().find("TestTrigger") ? scene().find("TestTrigger")->getComponent<EventProbe>() : nullptr)
            log("trigger after standing in it: " + std::to_string(probe->triggerEnters) + " enter(s): " + probe->lastTrigger);
        log("after pickup: " + std::to_string(scene().findAllWithTag("collectible").size()) + " left, collected " +
            std::to_string(gameState().collected) + "/" + std::to_string(gameState().total));
        if (auto* w = static_cast<vfx::Weather*>(scene().find("Weather"))) w->setWeather(vfx::Weather::Type::Rain, 2.0f);
        // Throw a rock at the crate pyramid from 6 m away (crates count their collisions).
        auto crates = scene().findAllWithTag("crate");
        for (auto* c : crates)
            if (!c->getComponent<EventProbe>()) c->addComponent<EventProbe>();
        m_crateStart.clear();
        glm::vec3 top(0.0f, -1e9f, 0.0f); // the top crate: nothing resting on it
        for (auto* c : crates) {
            m_crateStart.push_back(c->transform().position());
            if (c->transform().position().y > top.y) top = c->transform().position();
        }
        if (auto* p = static_cast<Player*>(scene().find("Player"))) {
            // Aim a little above it to allow for the drop over 6 m.
            const glm::vec3 eye = top + glm::vec3(0.0f, 0.4f, 6.0f);
            const glm::vec3 d = glm::normalize(top + glm::vec3(0.0f, 0.75f, 0.0f) - eye);
            p->setPose(eye - glm::vec3(0.0f, p->eyeHeight, 0.0f), glm::degrees(std::atan2(-d.x, -d.z)),
                       glm::degrees(std::asin(d.y)));
            m_throwPending = true;
        }
        ++m_step;
    } else if (m_step == 2 && m_throwPending && m_t > 4.2f) {
        // (one frame later, once the head transform follows the new pose)
        if (auto* p = static_cast<Player*>(scene().find("Player"))) {
            p->throwRock();
        }
        m_throwPending = false;
    } else if (m_step == 2 && !m_throwPending && m_t > 7.0f) {
        auto crates = scene().findAllWithTag("crate");
        int moved = 0;
        for (size_t i = 0; i < crates.size() && i < m_crateStart.size(); ++i) {
            if (glm::length(crates[i]->transform().position() - m_crateStart[i]) > 0.2f) ++moved;
        }
        log("crates knocked by the thrown rock: " + std::to_string(moved) + " / " + std::to_string(crates.size()));
        log(std::string("object spawned from onDestroy(): ") + (scene().find("SpawnedFromOnDestroy") ? "yes" : "NO") +
            ", destroyed one gone: " + (scene().find("SpawnOnDestroy") ? "NO" : "yes"));
        int enters = 0, stays = 0, exits = 0;
        float impact = 0.0f;
        for (auto* c : crates) {
            if (auto* probe = c->getComponent<EventProbe>()) {
                enters += probe->enters;
                stays += probe->stays;
                exits += probe->exits;
                impact = std::max(impact, probe->maxImpact);
            }
        }
        log("crate collision events: " + std::to_string(enters) + " enter, " + std::to_string(stays) + " stay, " +
            std::to_string(exits) + " exit, max impact " + std::to_string(impact) + " m/s");
        if (auto* t = scene().find("TestTrigger"))
            if (auto* probe = t->getComponent<EventProbe>())
                log("trigger box: " + std::to_string(probe->triggerEnters) + " enter(s) (" + probe->lastTrigger + "), " +
                    std::to_string(probe->triggerExits) + " exit(s)");
        if (auto* pl = scene().find("Player"))
            if (auto* probe = pl->getComponent<EventProbe>())
                log("player saw " + std::to_string(probe->triggerEnters) + " trigger enter(s) ('" + probe->lastTrigger +
                    "'), " + std::to_string(probe->triggerExits) + " exit(s)");
        if (auto* w = static_cast<vfx::Weather*>(scene().find("Weather"))) {
            log(std::string("weather now: ") + vfx::Weather::name(w->weather()) + ", rain=" + std::to_string(w->current().rain));
        }
        for (auto* rock : scene().findAllWithTag("thrown")) {
            auto* rb = rock->getComponent<ve::Rigidbody>();
            const glm::vec3 p = rock->transform().position();
            log("thrown rock at y=" + std::to_string(p.y) + " (ground " + std::to_string(m_terrain->height(p.x, p.z)) +
                "), speed " + std::to_string(rb ? glm::length(rb->linearVelocity()) : -1.0f));
        }
        log("physics bodies: " + std::to_string(physics().bodyCount()) + ", step ms: " + std::to_string(physics().lastStepMs));
        const ve::InstanceStats& is = renderer().instances().stats();
        log("instances: " + std::to_string(is.instances) + ", camera draws per LOD " + std::to_string(is.visibleLod[0]) +
            "/" + std::to_string(is.visibleLod[1]) + "/" + std::to_string(is.visibleLod[2]) + ", shadow draws " +
            std::to_string(is.visibleShadow));
        log("objects: " + std::to_string(scene().objectCount()) + ", draw calls: " +
            std::to_string(renderer().stats().drawCalls) + ", gpu ms: " + std::to_string(renderer().gpuFrameTimeMs()));
        // Foliage streaming: jump to the far corner and let cells stream.
        auto* foliageObject = dynamic_cast<ve::Foliage*>(scene().find("Foliage"));
        auto* foliage = foliageObject ? foliageObject->system() : nullptr;
        auto* player = static_cast<Player*>(scene().find("Player"));
        if (foliage && player) {
            const ve::FoliageStats& fs = foliage->stats();
            if (auto* fire = scene().find("Campfire"))
                if (auto* src = fire->getComponent<ve::AudioSource>())
                    log("audio: fire occlusion in the open: " + std::to_string(src->occluded()));
            log("foliage before: " + std::to_string(fs.instances) + " instances, " + std::to_string(fs.cells) + " cells, " +
                std::to_string(fs.bodies) + " bodies");
            const glm::vec2 corner(-240.0f, -240.0f);
            player->teleport({corner.x, m_terrain->height(corner.x, corner.y), corner.y}, 0.0f);
            m_t = 0.0f;
            ++m_step;
        } else {
            engine().quit();
            m_step = 100;
        }
    } else if (m_step == 3 && m_t > 4.0f) {
        auto* foliageObject = dynamic_cast<ve::Foliage*>(scene().find("Foliage"));
        ve::FoliageSystem* foliage = foliageObject->system();
        const ve::FoliageStats& fs = foliage->stats();
        log("foliage after teleport: " + std::to_string(fs.instances) + " instances, " + std::to_string(fs.cells) +
            " cells, " + std::to_string(fs.pending) + " pending, " + std::to_string(fs.bodies) + " bodies, " +
            std::to_string(fs.dropped) + " dropped, " + std::to_string(fs.generateMs) + " ms per cell");
        // Bushes are deterministic: the instances near the player must match a fresh query.
        const glm::vec3 p = static_cast<Player*>(scene().find("Player"))->transform().position();
        logAudio("far corner");
        audio().music().pushMood("Combat", 10, 1.0f); // as a fight would
        if (auto* dn = dynamic_cast<vfx::DayNightCycle*>(scene().find("DayNight"))) dn->timeOfDay = 22.0f;
        log("bushes within 30 m (query): " + std::to_string(foliage->query(uint32_t(foliageObject->layerIndex("Bushes")), glm::vec2(p.x, p.z), 30.0f).size()));
        // Audio: step inside a hut (zone muffles the Weather bus) and check the fire's occlusion.
        if (auto* hutZone = scene().find("HutZone")) {
            const glm::vec3 hp = hutZone->transform().position();
            static_cast<Player*>(scene().find("Player"))->teleport(hp + glm::vec3(0.0f, 0.2f, 0.0f), 0.0f);
        }
        m_t = 0.0f;
        ++m_step;
    } else if (m_step == 4 && m_t > 2.0f) {
        const ve::AudioBus& weather = audio().bus("Weather");
        int inside = 0;
        scene().forEach<ve::AudioZone>([&](ve::AudioZone& z) { inside += z.listenerInside() ? 1 : 0; });
        float fireOcclusion = -1.0f;
        if (auto* fire = scene().find("Campfire"))
            if (auto* src = fire->getComponent<ve::AudioSource>()) fireOcclusion = src->occluded();
        log("audio: inside " + std::to_string(inside) + " hut zone(s), Weather bus x" + std::to_string(weather.zoneVolume) +
            " / " + std::to_string(weather.zoneLowpass) + " Hz, fire occlusion " + std::to_string(fireOcclusion));
        logAudio("hut, 22:00, combat pushed");
        audio().music().popMood("Combat");
        log("audio: combat popped -> mood '" + audio().music().currentMood() + "'");
        // The big world: walk into the desert, then the jungle (terrain LOD,
        // physics tiles and foliage all stream around the player).
        if (auto* dn = dynamic_cast<vfx::DayNightCycle*>(scene().find("DayNight"))) dn->timeOfDay = 12.0f;
        static_cast<Player*>(scene().find("Player"))->teleport(groundAt({1200.0f, -300.0f}), 0.0f);
        m_t = 0.0f;
        ++m_step;
    } else if ((m_step == 5 || m_step == 6) && m_t > 4.0f) {
        const bool desert = m_step == 5;
        const char* biome = desert ? "Desert" : "Jungle";
        auto* player = static_cast<Player*>(scene().find("Player"));
        const glm::vec3 p = player->transform().position();
        auto* foliageObject = dynamic_cast<ve::Foliage*>(scene().find("Foliage"));
        const int layer = foliageObject ? foliageObject->layerIndex(desert ? "Quiver trees" : "Jungle trees") : -1;
        const size_t trees = layer >= 0 ? foliageObject->system()->query(uint32_t(layer), glm::vec2(p.x, p.z), 100.0f).size() : 0;
        log(std::string(biome) + ": weight " + std::to_string(m_terrain->biomeWeight(biome, p.x, p.z)) + ", player grounded: " +
            (player->grounded() ? "yes" : "NO") + " (feet " + std::to_string(p.y) + ", ground " +
            std::to_string(m_terrain->height(p.x, p.z)) + "), " + std::to_string(trees) + " " +
            (desert ? "quiver trees" : "jungle trees") + " within 100 m, terrain nodes " + std::to_string(m_terrain->drawnNodes()) +
            ", collider tiles " + std::to_string(m_terrain->colliderTiles()) + ", foliage " +
            std::to_string(foliageObject ? foliageObject->system()->stats().instances : 0) + " instances");
        if (desert) {
            player->teleport(groundAt({-1150.0f, 450.0f}), 0.0f);
        } else {
            // Interactive foliage: stand next to an acacia near the camp.
            const int layer = foliageObject ? foliageObject->layerIndex("Acacias") : -1;
            const auto tree = layer >= 0 ? foliageObject->system()->nearest(uint32_t(layer), {90.0f, 60.0f}, 150.0f) : std::nullopt;
            if (!tree) {
                log("interactive foliage: no acacia found");
                engine().quit();
            } else {
                m_tree = tree->id;
                // 7 m east of it (inside its promoteDistance), looking west at it.
                player->setPose(groundAt(glm::vec2(tree->position.x + 7.0f, tree->position.z)), 90.0f, 8.0f);
            }
        }
        m_t = 0.0f;
        ++m_step;
    } else if (m_step == 7 && m_t > 1.0f) {
        shot("1-acacia");
        // Step up to the trunk and look at it: the player's Interactor should focus it.
        if (const auto info = scene().findFirst<ve::Foliage>()->system()->instance(m_tree))
            static_cast<Player*>(scene().find("Player"))->setPose(groundAt(glm::vec2(info->position.x + 1.3f, info->position.z)), 90.0f, -10.0f);
        m_t = 0.0f;
        ++m_step;
    } else if (m_step == 8 && m_t > 0.3f) {
        ve::FoliageSystem* foliage = scene().findFirst<ve::Foliage>()->system();
        auto* tree = dynamic_cast<ChoppableTree*>(foliage->promoted(m_tree));
        log(std::string("acacia promoted to a ChoppableTree: ") + (tree ? "yes" : "NO"));
        // Through the engine's interaction system, like pressing E.
        auto* who = scene().find("Player")->getComponent<ve::Interactor>();
        const ve::Interactable* focus = who ? who->focus() : nullptr;
        log("interactor focus: " + std::string(focus && &focus->gameObject() == tree ? "the acacia" : focus ? focus->gameObject().name() : "nothing") +
            ", prompt '" + (who ? who->prompt() : std::string()) + "'");
        int uses = 0;
        for (int i = 0; who && i < ChoppableTree::kChops; ++i) uses += who->interact() ? 1 : 0;
        const bool felled = foliage->state(m_tree) != 0;
        log("chopped " + std::to_string(uses) + " times: felled " + std::to_string(felled ? 1 : 0) + ", state '" +
            stateLabel(*foliage, m_tree) + "', wood " + std::to_string(gameState().wood));
        static_cast<Player*>(scene().find("Player"))->teleport(groundAt({1200.0f, -300.0f}), 0.0f); // far away: the cell unloads
        m_t = 0.0f;
        ++m_step;
    } else if (m_step == 9 && m_t > 2.0f) {
        const auto info = scene().findFirst<ve::Foliage>()->system()->instance(m_tree);
        static_cast<Player*>(scene().find("Player"))->setPose(groundAt(glm::vec2(info->position.x + 3.0f, info->position.z)), 90.0f, -28.0f);
        m_t = 0.0f;
        ++m_step;
    } else if (m_step == 10 && m_t > 2.0f) {
        shot("2-stump");
        m_t = 0.0f;
        ++m_step;
    } else if (m_step == 11 && m_t > 0.3f) {
        ve::FoliageSystem* foliage = scene().findFirst<ve::Foliage>()->system();
        auto* obj = foliage->promoted(m_tree);
        auto* link = obj ? obj->getComponent<ve::FoliageLink>() : nullptr;
        log("back at the tree: state '" + stateLabel(*foliage, m_tree) + "', promoted object shows '" +
            (link ? link->stateName() : std::string("none")) + "', " + std::to_string(foliage->overrides().size()) +
            " override(s) to save");
        // Berries: pick a bush, then let them grow back.
        auto* foliageObject = scene().findFirst<ve::Foliage>();
        const int layer = foliageObject->layerIndex("Berry bushes");
        const auto bush = layer >= 0 ? foliage->nearest(uint32_t(layer), {60.0f, 40.0f}, 400.0f) : std::nullopt;
        if (!bush) {
            log("interactive foliage: no berry bush found");
            engine().quit();
        } else {
            m_bush = bush->id;
            static_cast<Player*>(scene().find("Player"))->setPose(groundAt(glm::vec2(bush->position.x + 2.5f, bush->position.z)), 90.0f, -25.0f);
            if (Regrowth* r = Regrowth::find(scene())) r->regrowSeconds = 2.0f;
        }
        m_t = 0.0f;
        ++m_step;
    } else if (m_step == 12 && m_t > 1.0f) {
        shot("3-berry-bush");
        m_t = 0.0f;
        ++m_step;
    } else if (m_step == 13 && m_t > 0.3f) {
        ve::FoliageSystem* foliage = scene().findFirst<ve::Foliage>()->system();
        auto* bush = dynamic_cast<BerryBush*>(foliage->promoted(m_bush));
        const bool picked = bush && bush->pick();
        log(std::string("berry bush promoted: ") + (bush ? "yes" : "NO") + ", picked: " + (picked ? "yes" : "no") + ", state '" +
            stateLabel(*foliage, m_bush) + "', berries " + std::to_string(gameState().berries));
        m_t = 0.0f;
        ++m_step;
    } else if (m_step == 14 && m_t > 3.0f) {
        ve::FoliageSystem* foliage = scene().findFirst<ve::Foliage>()->system();
        log("after the regrowth time: berry bush state '" + stateLabel(*foliage, m_bush) + "'");
        m_t = 0.0f;
        ++m_step;
    } else if (m_step == 14 && m_t > 0.5f) {
        shot("4-berries-picked");
    } else if ((m_step == 15 || m_step == 16) && m_t > 1.5f) {
        // F2: the camp villagers (skinned, animated) wander and swing their legs.
        std::string line;
        size_t i = 0;
        for (const auto& obj : scene().objects()) {
            auto* vp = dynamic_cast<Villager*>(obj.get());
            if (!vp || vp->isDestroyed()) continue;
            Villager& v = *vp;
            const ve::Animator* a = v.animator();
            const ve::GameObject* thigh = nullptr;
            forSubtree(v.transform(), [&](ve::Transform& t) {
                if (!thigh && t.gameObject().name() == "UpperLeg.L") thigh = &t.gameObject();
            });
            const float angle = thigh ? glm::degrees(glm::angle(thigh->transform().localRotation())) : -1.0f;
            const glm::vec3 p = v.transform().position();
            float top = 0.0f;
            forSubtree(v.transform(), [&](ve::Transform& t) {
                if (const auto* sm = t.gameObject().getComponent<ve::SkinnedMeshRenderer>(); sm && sm->mesh) top = sm->mesh->bounds().max.y;
            });
            line += (i++ ? "; " : "") + std::string("clip '") + (a ? a->current() : "?") + "' thigh " + std::to_string(angle) + " deg at (" +
                    std::to_string(p.x) + ", " + std::to_string(p.z) + "), skinned top " + std::to_string(top);
        }
        log("villagers " + std::to_string(i) + ": " + line + "; skinned this frame " + std::to_string(renderer().skinning().skinnedThisFrame()));
        if (m_step == 15) shot("5-villagers");
        if (m_step == 16) {
            // Interaction without a collider: face a sunstone 2 m away (the Interactor's view cone).
            auto stones = scene().findAllWithTag("collectible");
            if (!stones.empty()) {
                const glm::vec3 s = stones.front()->transform().position();
                auto* player = static_cast<Player*>(scene().find("Player"));
                player->setPose(groundAt(glm::vec2(s.x + 2.0f, s.z)), 90.0f, -12.0f);
            }
        }
        m_t = 0.0f;
        ++m_step;
    } else if (m_step == 17 && m_t > 0.5f) {
        auto* who = scene().find("Player")->getComponent<ve::Interactor>();
        const int before = gameState().collected;
        const std::string prompt = who ? who->prompt() : std::string();
        const bool used = who && who->interact();
        log("sunstone via the interactor: prompt '" + prompt + "', used " + (used ? "yes" : "no") + ", collected " +
            std::to_string(before) + " -> " + std::to_string(gameState().collected));
        // Save games: save now, change things, load, and check they're back.
        const GameState& gs = gameState();
        s_expect = {gs.collected, gs.wood, gs.berries, Sunstone::remaining(scene()), scene().find("Player")->transform().position(), m_tree.packed()};
        log(std::string("saved: ") + (saveProgress(scene(), "autotest") ? "yes" : "NO") + " (collected " + std::to_string(gs.collected) +
            ", wood " + std::to_string(gs.wood) + ", " + std::to_string(s_expect.stones) + " stones left)");
        m_t = 0.0f;
        ++m_step;
    } else if (m_step == 18 && m_t > 0.5f) {
        auto stones = scene().findAllWithTag("collectible");
        if (!stones.empty()) static_cast<Sunstone*>(stones.front())->collect();
        gameState().wood += 5;
        static_cast<Player*>(scene().find("Player"))->teleport(groundAt({1200.0f, -300.0f}), 0.0f);
        log("after the save: collected " + std::to_string(gameState().collected) + ", wood " + std::to_string(gameState().wood) +
            ", player moved 1.2 km");
        m_t = 0.0f;
        ++m_step;
    } else if (m_step == 19 && m_t > 0.5f) {
        s_resume = 20; // the reloaded level's AutoTest carries on
        log(std::string("loading the save (level rebuilt behind the loading screen): ") + (loadProgress("autotest") ? "started" : "NO SAVE"));
        ++m_step;
    } else if (m_step == 20 && m_t > 3.0f) {
        const GameState& gs = gameState();
        const glm::vec3 p = scene().find("Player")->transform().position();
        auto* foliage = scene().findFirst<ve::Foliage>();
        const ve::FoliageInstanceId tree = ve::FoliageInstanceId::unpack(s_expect.tree);
        const std::string treeState = foliage && foliage->system() ? stateLabel(*foliage->system(), tree) : "?";
        log("after loading: collected " + std::to_string(gs.collected) + " (saved " + std::to_string(s_expect.collected) + "), wood " +
            std::to_string(gs.wood) + " (" + std::to_string(s_expect.wood) + "), stones left " + std::to_string(Sunstone::remaining(scene())) +
            " (" + std::to_string(s_expect.stones) + "), player " + std::to_string(glm::distance(p, s_expect.player)) +
            " m from the saved spot, chopped acacia '" + treeState + "'");
        ve::save::remove("autotest");
        // Gamepad: a virtual SDL controller drives the player (stick forward, then Y = stop flying).
        const bool pad = input().attachVirtualGamepad();
        m_padStart = scene().find("Player")->transform().position();
        input().setVirtualGamepadAxis(ve::GamepadAxis::LeftY, -1.0f); // stick fully forward
        log(std::string("virtual gamepad: ") + (pad ? "attached" : "NOT AVAILABLE") + ", connected '" + input().gamepadName() + "'");
        m_t = 0.0f;
        ++m_step;
    } else if (m_step == 21 && m_t > 1.0f) {
        auto* player = static_cast<Player*>(scene().find("Player"));
        const float moved = glm::distance(player->transform().position(), m_padStart);
        log("gamepad: MoveY " + std::to_string(input().axisValue("MoveY")) + ", moved " + std::to_string(moved) + " m in 1 s (flying " +
            (player->flying() ? "yes" : "no") + ")");
        input().setVirtualGamepadAxis(ve::GamepadAxis::LeftY, 0.0f);
        input().setVirtualGamepadButton(ve::GamepadButton::Y, true); // ToggleFly
        m_t = 0.0f;
        ++m_step;
    } else if (m_step == 22 && m_t > 0.3f) {
        input().setVirtualGamepadButton(ve::GamepadButton::Y, false);
        auto* player = static_cast<Player*>(scene().find("Player"));
        std::string names;
        for (const std::string& n : input().bindingNames("ToggleFly")) names += (names.empty() ? "" : ", ") + n;
        log(std::string("gamepad: Y pressed -> flying ") + (player->flying() ? "yes" : "no") + "; ToggleFly is bound to " + names);
        // Bindings survive a JSON round trip (the F10 controls file).
        const ve::Json saved = input().saveBindings();
        input().clearAction("ToggleFly");
        input().loadBindings(saved);
        log("bindings JSON round trip: ToggleFly has " + std::to_string(input().bindingNames("ToggleFly").size()) + " binding(s)");
        m_t = 0.0f;
        ++m_step;
    } else if (m_step == 23 && m_t > 0.5f) {
        // Navigation: the level's navmesh (built behind the loading screen) routes around huts.
        ve::NavMeshSurface* nav = ve::NavMeshSurface::find(scene());
        if (!nav) {
            log("navmesh: NONE");
            engine().quit();
            return;
        }
        const auto& st = nav->stats();
        log("navmesh: ready " + std::string(nav->ready() ? "yes" : "no") + ", tiles " + std::to_string(st.tiles) + ", polygons " +
            std::to_string(st.polygons) + ", " + std::to_string(st.lastTileMs) + " ms per tile");
        ve::GameObject* hut = scene().find("Hut");
        if (hut) {
            const glm::vec3 h = hut->transform().position();
            const glm::vec3 a = groundAt({h.x - 6.0f, h.z}), b = groundAt({h.x + 6.0f, h.z});
            const ve::NavPath path = nav->findPath(a, b);
            const auto blocked = nav->raycast(a, b);
            log("path across a hut: complete " + std::string(path.complete ? "yes" : "no") + ", " + std::to_string(path.corners.size()) +
                " corners, " + std::to_string(path.length()) + " m (straight 12 m), straight line blocked " + (blocked ? "yes" : "no"));
        }
        // A villager follows the player (the game's AI on the engine's NavAgent).
        Villager* v = nullptr;
        for (const auto& obj : scene().objects())
            if (!v && !obj->isDestroyed()) v = dynamic_cast<Villager*>(obj.get());
        auto* player = static_cast<Player*>(scene().find("Player"));
        if (v && player) {
            const glm::vec3 vp = v->transform().position();
            const glm::vec3 goal = nav->randomPointNear(vp + glm::vec3(9.0f, 0.0f, 8.0f), 3.0f).value_or(vp + glm::vec3(9.0f, 0.0f, 8.0f));
            player->setPose(goal, 0.0f, -10.0f);
            v->follow(*player);
            m_followStart = glm::distance(vp, goal);
            m_follower = v->id();
            log("villager follows: start " + std::to_string(m_followStart) + " m from the player, on navmesh " +
                (v->agent().onNavMesh() ? "yes" : "no"));
        }
        m_t = 0.0f;
        ++m_step;
    } else if (m_step == 24 && m_t > 9.0f) {
        auto* v = static_cast<Villager*>(scene().findById(m_follower));
        auto* player = static_cast<Player*>(scene().find("Player"));
        if (v && player) {
            const glm::vec3 vp = v->transform().position(), pp = player->transform().position();
            log("villager after 9 s: " + std::to_string(glm::distance(glm::vec2(vp.x, vp.z), glm::vec2(pp.x, pp.z))) + " m from the player (was " +
                std::to_string(m_followStart) + "), following " + (v->mode() == Villager::Mode::Follow ? "yes" : "no") + ", walking " +
                (v->walking() ? "yes" : "no"));
            // Look at the villager from a few metres back for the screenshot, navmesh drawn.
            const glm::vec3 d = vp - pp;
            player->setPose(pp - glm::normalize(glm::vec3(d.x, 0.0f, d.z)) * 3.0f + glm::vec3(0.0f, 1.0f, 0.0f),
                            glm::degrees(std::atan2(-d.x, -d.z)), -18.0f);
            if (ve::NavMeshSurface* nav = ve::NavMeshSurface::find(scene())) nav->showDebug = true;
        }
        m_t = 0.0f;
        ++m_step;
    } else if (m_step == 25 && m_t > 0.5f) {
        shot("6-villager-follows");
        if (m_t > 1.0f) {
            engine().quit();
            ++m_step;
        }
    }
}

// -----------------------------------------------------------------------------
// Water
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// HUD
// -----------------------------------------------------------------------------

Hud::Hud(Player& player) : GameObject("HUD"), m_player(player)
{
}

void Hud::findEnvironment()
{
    auto find = [&]<class T>(T*& ptr, uint64_t& id) {
        ptr = dynamic_cast<T*>(scene().findById(id));
        if (!ptr) ptr = scene().findFirst<T>();
        id = ptr ? ptr->id() : 0;
    };
    find(m_weather, m_weatherId);
    find(m_dayNight, m_dayNightId);
}

bool saveProgress(ve::Scene& scene, const std::string& slot)
{
    const GameState& gs = gameState();
    const bool ok = ve::save::saveGame(scene, slot, {{"collected", gs.collected}, {"wood", gs.wood}, {"berries", gs.berries}});
    gameState().message = ok ? "Game saved" : "Saving failed";
    gameState().messageTimer = 2.0f;
    return ok;
}

void applyGameData(const ve::Json& game)
{
    GameState& gs = gameState();
    gs.collected = game.value("collected", 0);
    gs.wood = game.value("wood", 0);
    gs.berries = game.value("berries", 0);
    gs.message = "Game loaded";
    gs.messageTimer = 2.0f;
}

bool loadProgress(const std::string& slot)
{
    const bool ok = reloadWithSave(ve::Engine::get(), slot);
    if (!ok) {
        gameState().message = "No saved game";
        gameState().messageTimer = 2.0f;
    }
    return ok;
}

void loadControls()
{
    std::ifstream in(ve::save::folder() / "controls.json");
    if (!in) return;
    try {
        ve::Json j;
        in >> j;
        ve::Engine::get().input().loadBindings(j);
    } catch (const std::exception& e) {
        ve::log::warn("controls.json: {}", e.what());
    }
}

void saveControls()
{
    std::error_code ec;
    std::filesystem::create_directories(ve::save::folder(), ec);
    std::ofstream out(ve::save::folder() / "controls.json");
    out << ve::Engine::get().input().saveBindings().dump(1) << '\n';
}

void Hud::drawControls()
{
    ve::Input& in = input();
    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Controls (F10)", &m_showControls)) {
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("Gamepad: %s", in.gamepadConnected() ? in.gamepadName().c_str() : "none connected");
    if (!in.rebinding().empty()) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Press a key, mouse or gamepad button for '%s' (Esc cancels)",
                                                    in.rebinding().c_str());
    if (ImGui::BeginTable("bindings", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        for (const std::string& action : in.actions()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(action.c_str());
            ImGui::TableNextColumn();
            std::string names;
            for (const std::string& n : in.bindingNames(action)) names += (names.empty() ? "" : ", ") + n;
            ImGui::TextUnformatted(names.c_str());
            ImGui::TableNextColumn();
            ImGui::PushID(action.c_str());
            if (ImGui::SmallButton("Rebind")) in.startRebind(action);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Keyboard/mouse and gamepad bindings are replaced separately.");
    ImGui::End();
}

void Hud::update(float dt)
{
    // SAVANNA_CONTROLS=1: open the controls window (screenshots).
    static const bool s_controlsShot = std::getenv("SAVANNA_CONTROLS") != nullptr;
    if (s_controlsShot && !m_showControls && !m_controlsShotDone) {
        m_showControls = m_controlsShotDone = true;
        input().setMouseCaptured(false);
    }
    // A rebind just finished (F10): keep it for next time.
    const bool rebinding = !input().rebinding().empty();
    if (m_wasRebinding && !rebinding) saveControls();
    m_wasRebinding = rebinding;
    if (input().keyPressed(ve::Key::F10)) {
        m_showControls = !m_showControls;
        input().setMouseCaptured(!m_showControls);
    }
    gameState().total = gameState().collected + Sunstone::remaining(scene()); // stones can be added in the editor
    GameState& gs = gameState();
    gs.messageTimer = std::max(0.0f, gs.messageTimer - dt);
    findEnvironment();
    if (input().keyPressed(ve::Key::R) && m_weather) m_weather->next(12.0f);
    if (input().keyPressed(ve::Key::H)) m_showHelp = !m_showHelp;
    if (input().keyPressed(ve::Key::F5)) saveProgress(scene(), "quick");
    if (input().keyPressed(ve::Key::F9)) loadProgress("quick");
    // A stand-in for a fight until there are enemies: game code pushes a mood
    // when combat starts and pops it when it ends; the music returns to
    // whatever was playing (camp or explore).
    if (input().keyPressed(ve::Key::M)) {
        m_combat = !m_combat;
        if (m_combat) audio().music().pushMood("Combat", 10, 1.0f);
        else audio().music().popMood("Combat", 4.0f);
        gs.message = m_combat ? "Combat music (M to stop)" : "Combat over";
        gs.messageTimer = 2.0f;
    }
}

void Hud::onGui()
{
    if (m_showControls) drawControls();
    if (engine().editor() && engine().editor()->paused()) return; // the editor's panels take the screen
    if (vfx::PhotoMode::anyActive()) return;                         // a clean view to compose the shot
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 c(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);

    // Crosshair.
    if (input().mouseCaptured()) {
        dl->AddCircleFilled(c, 2.5f, IM_COL32(255, 255, 255, 200));
        dl->AddCircle(c, 3.5f, IM_COL32(0, 0, 0, 120));
    }

    // Status panel (top right).
    const GameState& gs = gameState();
    const float tod = m_dayNight ? m_dayNight->timeOfDay : 12.0f;
    const int hours = static_cast<int>(tod);
    const int minutes = static_cast<int>((tod - hours) * 60.0f);
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%02d:%02d   %s%s\nSunstones: %d / %d\nWood: %d   Berries: %d", hours, minutes,
                  m_weather ? vfx::Weather::name(m_weather->weather()) : "-", m_player.flying() ? "   [FLY]" : "", gs.collected,
                  gs.total, gs.wood, gs.berries);
    const ImVec2 size = ImGui::CalcTextSize(buf);
    const ImVec2 p(io.DisplaySize.x - size.x - 20.0f, 16.0f);
    dl->AddRectFilled({p.x - 10, p.y - 6}, {p.x + size.x + 10, p.y + size.y + 6}, IM_COL32(20, 14, 8, 140), 6.0f);
    dl->AddText(p, IM_COL32(255, 235, 200, 255), buf);

    // What are we looking at? A physics raycast from the eye (ignores the player's own capsule).
    if (input().mouseCaptured()) {
        if (auto hit = physics().raycast(m_player.eyePosition(), m_player.lookDirection(), 25.0f)) {
            // Instanced props have no GameObject; their collider carries a prop id instead.
            const std::string name = hit->gameObject ? hit->gameObject->name() : std::string(propName(hit->userValue));
            if (!name.empty() && name != "Terrain") {
                char label[96];
                std::snprintf(label, sizeof(label), "%s  (%.1f m)", name.c_str(), hit->distance);
                const ImVec2 ls = ImGui::CalcTextSize(label);
                dl->AddText({c.x - ls.x * 0.5f, c.y + 14.0f}, IM_COL32(255, 255, 255, 170), label);
            }
        }
    }

    // Centre message: the interaction prompt (what the player looks at), or the last event.
    std::string centre = gs.messageTimer > 0.0f ? gs.message : std::string();
    if (const auto* who = m_player.getComponent<ve::Interactor>(); who && centre.empty())
        if (const std::string p = who->prompt(); !p.empty()) centre = "[E] " + p;
    if (!centre.empty()) {
        const std::string& message = centre;
        const ImVec2 ms = ImGui::CalcTextSize(message.c_str());
        const ImVec2 mp(c.x - ms.x * 0.5f, c.y + 40.0f);
        dl->AddRectFilled({mp.x - 8, mp.y - 4}, {mp.x + ms.x + 8, mp.y + ms.y + 4}, IM_COL32(20, 14, 8, 150), 5.0f);
        dl->AddText(mp, IM_COL32(255, 220, 150, 255), message.c_str());
    }

    if (m_showHelp) {
        const char* help = "WASD move  Shift sprint  Space jump  F fly  E pick up  Q/RMB throw\n"
                           "T/G time +/-  R weather  M combat music  Tab mouse  F5/F9 save/load  F10 controls  F1 debug  F12 photo mode  H hide help";
        const ImVec2 hs = ImGui::CalcTextSize(help);
        const ImVec2 hp(16.0f, io.DisplaySize.y - hs.y - 16.0f);
        dl->AddRectFilled({hp.x - 8, hp.y - 6}, {hp.x + hs.x + 8, hp.y + hs.y + 6}, IM_COL32(20, 14, 8, 120), 6.0f);
        dl->AddText(hp, IM_COL32(240, 230, 210, 230), help);
    }
}

} // namespace savanna
