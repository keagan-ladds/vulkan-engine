#include "Props.hpp"

#include "DayNight.hpp"
#include "Noise2D.hpp"
#include "Player.hpp"

#include <ve/assets/AssetManager.hpp>
#include <ve/audio/Audio.hpp>
#include <ve/core/Engine.hpp>
#include <ve/core/Input.hpp>
#include <ve/core/Time.hpp>
#include <ve/render/Mesh.hpp>
#include <ve/render/Primitives.hpp>
#include <ve/render/Renderer.hpp>
#include <ve/scene/Components.hpp>
#include <ve/scene/Scene.hpp>
#include <vfx/Particles.hpp>
#include <vfx/Weather.hpp>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include <cmath>
#include <cstdio>

namespace savanna {

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
           0.1f * (valueNoise(glm::vec2(t * 6.0f, seed)) - 0.5f);
}

vfx::ParticleSettings flameSettings(float scale)
{
    vfx::ParticleSettings s;
    s.rate = 70.0f * scale;
    s.maxParticles = 300;
    s.lifetimeMin = 0.35f;
    s.lifetimeMax = 0.8f;
    s.spawnRadius = 0.28f * scale;
    s.velocity = {0.0f, 1.6f * scale, 0.0f};
    s.velocityRandom = {0.25f, 0.4f, 0.25f};
    s.sizeStart = 0.35f * scale;
    s.sizeEnd = 0.08f * scale;
    s.colorStart = {5.0f, 2.2f, 0.6f, 0.9f};
    s.colorMid = {3.2f, 0.9f, 0.15f, 0.7f};
    s.colorEnd = {0.6f, 0.08f, 0.0f, 0.0f};
    s.blend = ve::BlendMode::Additive;
    s.rotationSpeed = 2.0f;
    s.softness = 0.2f;
    return s;
}

} // namespace

// -----------------------------------------------------------------------------
// Campfire
// -----------------------------------------------------------------------------

Campfire::Campfire(std::shared_ptr<ve::Material> propMaterial) : GameObject("Campfire"), m_material(std::move(propMaterial)) {}

void Campfire::start()
{
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

    // Flames, smoke and sparks.
    auto& flames = s.spawn("Flames");
    flames.setParent(this, false);
    flames.transform().setLocalPosition({0.0f, 0.2f, 0.0f});
    flames.addComponent<vfx::ParticleEmitter>().settings = flameSettings(1.0f);

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
    audioSrc.volume = 0.9f;
    audioSrc.minDistance = 1.5f;
    audioSrc.maxDistance = 28.0f;
}

void Campfire::update(float dt)
{
    m_t += dt;
    if (m_light) m_light->intensity = baseIntensity * flicker(m_t, 1.3f);
}

// -----------------------------------------------------------------------------
// Torch
// -----------------------------------------------------------------------------

Torch::Torch(std::shared_ptr<ve::Material> propMaterial) : GameObject("Torch"), m_material(std::move(propMaterial)) {}

void Torch::start()
{
    ve::MeshData md;
    md.append(ve::primitives::cylinder(0.05f, 0.04f, 1.9f, 6), glm::mat4(1.0f), {0.25f, 0.16f, 0.09f, 1.0f});
    md.append(ve::primitives::cylinder(0.09f, 0.11f, 0.25f, 6), glm::translate(glm::mat4(1.0f), {0, 1.85f, 0}),
              {0.12f, 0.09f, 0.07f, 1.0f});
    md.makeFlatShaded();
    addComponent<ve::MeshRenderer>(ve::Mesh::create(renderer(), md, {}, "torch"), m_material);

    auto& flame = scene().spawn("TorchFlame");
    flame.setParent(this, false);
    flame.transform().setLocalPosition({0.0f, 2.1f, 0.0f});
    flame.addComponent<vfx::ParticleEmitter>().settings = flameSettings(0.45f);
    m_light = &flame.addComponent<ve::PointLight>();
    m_light->color = {1.0f, 0.55f, 0.22f};
    m_light->range = 10.0f;
    m_light->castShadows = true;
    m_light->shadowBias = 0.006f;
    m_t = static_cast<float>(reinterpret_cast<uintptr_t>(this) % 1000);
}

void Torch::update(float dt)
{
    m_t += dt;
    if (m_light) m_light->intensity = 7.0f * flicker(m_t, 5.7f);
}

// -----------------------------------------------------------------------------
// Sunstone
// -----------------------------------------------------------------------------

Sunstone::Sunstone(std::shared_ptr<ve::Mesh> mesh, std::shared_ptr<ve::Material> material, Player& player,
                   std::shared_ptr<ve::AudioClip> pickupSound)
    : GameObject("Sunstone"), m_mesh(std::move(mesh)), m_material(std::move(material)), m_player(player),
      m_sound(std::move(pickupSound))
{
    tag = "collectible";
}

void Sunstone::start()
{
    m_base = transform().position();
    m_visual = &scene().spawn("SunstoneVisual");
    m_visual->setParent(this, false);
    m_visual->addComponent<ve::MeshRenderer>(m_mesh, m_material);
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

    // Pick up when close, roughly looked at, and "Use" pressed.
    const glm::vec3 toStone = m_visual->transform().position() - m_player.eyePosition();
    const float dist = glm::length(toStone);
    if (dist < 3.0f && glm::dot(toStone / dist, m_player.lookDirection()) > 0.6f) {
        GameState& gs = gameState();
        gs.message = "[E] Pick up sunstone";
        gs.messageTimer = 0.1f;
        if (m_player.usePressed()) collect();
    }
}

void Sunstone::collect()
{
    if (isDestroyed()) return;
    GameState& gs = gameState();
    ++gs.collected;
    gs.message = "Sunstone collected! (" + std::to_string(gs.collected) + "/" + std::to_string(gs.total) + ")";
    gs.messageTimer = 2.5f;
    if (m_sound) audio().playOneShot(*m_sound, 0.8f);
    destroy(); // deferred to the end of the frame; children (visual + light) go too
}

// -----------------------------------------------------------------------------
// AutoTest
// -----------------------------------------------------------------------------

void AutoTest::update(float dt)
{
    m_t += dt;
    auto log = [](const std::string& s) { std::fprintf(stderr, "[autotest] %s\n", s.c_str()); };
    if (m_step == 0 && m_t > 3.0f) {
        auto stones = scene().findAllWithTag("collectible");
        log("sunstones in scene: " + std::to_string(stones.size()));
        if (!stones.empty()) static_cast<Sunstone*>(stones.front())->collect();
        ++m_step;
    } else if (m_step == 1 && m_t > 4.0f) {
        log("after pickup: " + std::to_string(scene().findAllWithTag("collectible").size()) + " left, collected " +
            std::to_string(gameState().collected) + "/" + std::to_string(gameState().total));
        if (auto* w = static_cast<vfx::Weather*>(scene().find("Weather"))) w->setWeather(vfx::Weather::Type::Rain, 2.0f);
        ++m_step;
    } else if (m_step == 2 && m_t > 7.0f) {
        if (auto* w = static_cast<vfx::Weather*>(scene().find("Weather"))) {
            log(std::string("weather now: ") + vfx::Weather::name(w->weather()) + ", rain=" + std::to_string(w->current().rain));
        }
        log("objects: " + std::to_string(scene().objectCount()) + ", draw calls: " +
            std::to_string(renderer().stats().drawCalls) + ", gpu ms: " + std::to_string(renderer().gpuFrameTimeMs()));
        engine().quit();
        ++m_step;
    }
}

// -----------------------------------------------------------------------------
// Water
// -----------------------------------------------------------------------------

Water::Water() : GameObject("Water")
{
    ve::Renderer& r = renderer();
    ve::ShaderDesc desc;
    desc.name = "Water";
    desc.vertex = "water.vert";
    desc.fragment = "water.frag";
    desc.blend = ve::BlendMode::AlphaBlend;
    desc.queue = ve::RenderQueue::Transparent;
    desc.depthWrite = false;
    desc.castShadows = false;
    desc.cull = ve::CullMode::None;
    auto mat = r.createMaterial(r.createShader(desc));
    mat->setVector(0, {0.05f, 0.09f, 0.06f, 1.0f}); // deep colour (murky swamp water)
    mat->setVector(1, {0.22f, 0.25f, 0.15f, 1.0f}); // shallow colour
    mat->setVector(2, {1.2f, 0.5f, 0.35f, 0.12f});  // absorption, shore foam, ripple scale, ripple strength
    // The atmosphere's sky LUT is a render-graph texture; it's in the bindless
    // table like any other, so the material can just reference its index.
    const ve::TextureId lut = r.graph().texture("SkyViewLUT");
    mat->setTextureIndex(0, lut != ve::kNoTexture ? r.graph().get(lut).sampledIndex : ve::kInvalidIndex);

    auto mesh = ve::Mesh::create(r, ve::primitives::plane(640.0f, 640.0f, 8, 8), {}, "water");
    auto& mr = addComponent<ve::MeshRenderer>(mesh, mat);
    mr.castShadows = false;
}

// -----------------------------------------------------------------------------
// HUD
// -----------------------------------------------------------------------------

Hud::Hud(Player& player, DayNightCycle& dayNight, vfx::Weather& weather)
    : GameObject("HUD"), m_player(player), m_dayNight(dayNight), m_weather(weather)
{
}

void Hud::update(float dt)
{
    GameState& gs = gameState();
    gs.messageTimer = std::max(0.0f, gs.messageTimer - dt);
    if (input().keyPressed(ve::Key::R)) m_weather.next(12.0f);
    if (input().keyPressed(ve::Key::H)) m_showHelp = !m_showHelp;
}

void Hud::onGui()
{
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
    const int hours = static_cast<int>(m_dayNight.timeOfDay);
    const int minutes = static_cast<int>((m_dayNight.timeOfDay - hours) * 60.0f);
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%02d:%02d   %s%s\nSunstones: %d / %d", hours, minutes,
                  vfx::Weather::name(m_weather.weather()), m_player.flying() ? "   [FLY]" : "", gs.collected, gs.total);
    const ImVec2 size = ImGui::CalcTextSize(buf);
    const ImVec2 p(io.DisplaySize.x - size.x - 20.0f, 16.0f);
    dl->AddRectFilled({p.x - 10, p.y - 6}, {p.x + size.x + 10, p.y + size.y + 6}, IM_COL32(20, 14, 8, 140), 6.0f);
    dl->AddText(p, IM_COL32(255, 235, 200, 255), buf);

    // Centre message (pickup prompt etc).
    if (gs.messageTimer > 0.0f && !gs.message.empty()) {
        const ImVec2 ms = ImGui::CalcTextSize(gs.message.c_str());
        const ImVec2 mp(c.x - ms.x * 0.5f, c.y + 40.0f);
        dl->AddRectFilled({mp.x - 8, mp.y - 4}, {mp.x + ms.x + 8, mp.y + ms.y + 4}, IM_COL32(20, 14, 8, 150), 5.0f);
        dl->AddText(mp, IM_COL32(255, 220, 150, 255), gs.message.c_str());
    }

    if (m_showHelp) {
        const char* help = "WASD move  Shift sprint  Space jump  F fly  E pick up\n"
                           "T/G time +/-  R weather  Tab mouse  F1 debug  H hide help";
        const ImVec2 hs = ImGui::CalcTextSize(help);
        const ImVec2 hp(16.0f, io.DisplaySize.y - hs.y - 16.0f);
        dl->AddRectFilled({hp.x - 8, hp.y - 6}, {hp.x + hs.x + 8, hp.y + hs.y + 6}, IM_COL32(20, 14, 8, 120), 6.0f);
        dl->AddText(hp, IM_COL32(240, 230, 210, 230), help);
    }
}

} // namespace savanna
