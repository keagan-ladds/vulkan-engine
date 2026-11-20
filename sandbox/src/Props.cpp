#include "Props.hpp"

#include <ve/assets/AssetManager.hpp>
#include <ve/audio/Audio.hpp>
#include <ve/core/Engine.hpp>
#include <ve/core/Env.hpp>
#include <ve/gameplay/Interaction.hpp>
#include <ve/physics/Physics.hpp>
#include <ve/render/Material.hpp>
#include <ve/render/Mesh.hpp>
#include <ve/render/Primitives.hpp>
#include <ve/render/Renderer.hpp>
#include <ve/scene/Components.hpp>
#include <ve/scene/Reflection.hpp>
#include <ve/scene/Scene.hpp>
#include <vfx/Fire.hpp>
#include <vfx/Particles.hpp>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace savanna {

GameState& gameState()
{
    static GameState state;
    return state;
}

void notify(const std::string& text, float seconds)
{
    GameState& gs = gameState();
    gs.message = text;
    gs.messageTimer = seconds;
}

namespace {
// Live objects of one kind, by id (Campfire::all, Sunstone::remaining).
std::vector<uint64_t>& campfireIds()
{
    static std::vector<uint64_t> ids;
    return ids;
}
std::vector<uint64_t>& sunstoneIds()
{
    static std::vector<uint64_t> ids;
    return ids;
}
void forget(std::vector<uint64_t>& ids, uint64_t id) { std::erase(ids, id); }
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

const std::vector<uint64_t>& Campfire::all() { return campfireIds(); }

void Campfire::onDestroy() { forget(campfireIds(), id()); }

void Campfire::start()
{
    tag = "fire";
    campfireIds().push_back(id()); // standing in it hurts (Player)
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
    m_fire->haze = ve::env::number("SAVANNA_HAZE", m_fire->haze); // comparisons

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
    if (const auto* particles = r.graph().findPass<vfx::ParticlePass>()) smoke.texture = particles->smokeSprite; // (a game without effects: plain quads)

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

void Campfire::update(float)
{
    if (!m_light || !m_fire) return; // (both made in start())
    // The glow follows the flames: brightness and a small wobble (shadows dance).
    m_light->intensity = baseIntensity * m_fire->flicker();
    m_light->transform().setLocalPosition(glm::vec3(0.0f, 0.9f, 0.0f) + m_fire->lightJitter());
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
    m_fire->haze *= ve::env::number("SAVANNA_HAZE", 1.0f);
    // Each torch flickers differently, the same way every run (from where it stands).
    const glm::vec3 at = transform().position();
    m_fire->seed = uint32_t(int64_t(std::floor(at.x * 7.0f + at.z * 13.0f)) & 0x7fffffff) % 997u;
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
}

void Torch::update(float)
{
    if (!m_light || !m_fire) return; // (both made in start())
    m_light->intensity = intensity * m_fire->flicker();
    m_light->transform().setLocalPosition(glm::vec3(0.0f, 0.2f, 0.0f) + m_fire->lightJitter());
}

// -----------------------------------------------------------------------------
// Sunstone
// -----------------------------------------------------------------------------

int Sunstone::remaining(const ve::Scene& scene)
{
    // (The HUD asks every frame: a few ids, not a walk over every object.)
    int n = 0;
    for (uint64_t id : sunstoneIds()) n += dynamic_cast<const Sunstone*>(scene.findById(id)) ? 1 : 0;
    return n;
}

void Sunstone::onDestroy() { forget(sunstoneIds(), id()); }

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
    sunstoneIds().push_back(id());

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
    gs.inventory.add("sunstone", 1);
    gs.total = gs.collected + remaining(scene()) - 1; // this one is still in the scene until the frame ends
    notify("Sunstone collected! (" + std::to_string(gs.collected) + "/" + std::to_string(gs.total) + ")");
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

} // namespace savanna
