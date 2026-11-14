// =============================================================================
// Savanna - the engine's test scene.
//
// Builds a small open world that exercises every engine feature:
//   terrain/grass/water with custom shaders, glTF import, textures from PNG,
//   sun + moon cascaded shadows, point light cube shadows (campfire, torches),
//   sky + volumetric clouds + fog + bloom (ve_effects), particles, weather,
//   3D audio, ImGui HUD, and a fixed-timestep player controller.
// =============================================================================

#include "DayNight.hpp"
#include "Noise2D.hpp"
#include "Player.hpp"
#include "Props.hpp"
#include "Terrain.hpp"
#include "Vegetation.hpp"

#include <ve/ve.hpp>
#include <vfx/Atmosphere.hpp>
#include <vfx/Bloom.hpp>
#include <vfx/Particles.hpp>
#include <vfx/Vfx.hpp>
#include <vfx/VolumetricFog.hpp>
#include <vfx/Weather.hpp>

#include <glm/gtc/constants.hpp>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>

using namespace savanna;

namespace {

void buildWorld(ve::Engine& engine)
{
    ve::Renderer& r = engine.renderer();
    ve::Scene& scene = engine.scene();
    ve::AssetManager& assets = engine.assets();

    // --- Optional effects: sky, clouds, fog, particles, bloom ----------------------
    vfx::Effects fx = vfx::addDefaultEffects(r);
    fx.fog->density = 0.005f;
    fx.fog->heightFalloff = 0.06f;
    fx.bloom->intensity = 0.05f;

    // --- World ---------------------------------------------------------------------
    auto& terrain = scene.spawn<Terrain>();
    scene.spawn<Water>();
    const PropLibrary props = buildPropLibrary(r);
    scatterProps(scene, terrain, props);
    buildGrass(scene, r, terrain);

    // --- Weather + day/night ------------------------------------------------------------
    auto& weather = scene.spawn<vfx::Weather>();
    weather.rainSound = assets.loadAudio("audio/rain_loop.wav");
    weather.setWeather(vfx::Weather::Type::Hazy, 0.01f);
    auto& dayNight = scene.spawn<DayNightCycle>();
    dayNight.weather = &weather;

    // --- Camp: imported glTF huts around a campfire ------------------------------------
    const glm::vec2 camp = Terrain::kCamp;
    const float campY = terrain.height(camp.x, camp.y);
    auto& fire = scene.spawn<Campfire>(props.material);
    fire.transform().setPosition({camp.x, campY, camp.y});

    auto hut = assets.loadModel("models/rondavel.gltf");
    for (int i = 0; i < 3; ++i) {
        const float a = glm::radians(200.0f + 65.0f * static_cast<float>(i));
        const glm::vec2 p = camp + glm::vec2(std::cos(a), std::sin(a)) * 11.0f;
        // staticColliders: every hut mesh also becomes a static triangle-mesh
        // collider, so walls block you but the doorway shape is respected.
        ve::GameObject& h = scene.instantiate(*hut, nullptr, /*staticColliders*/ true);
        const float hy = terrain.height(p.x, p.y) - 0.05f;
        h.transform().setPosition({p.x, hy, p.y});
        // Turn the doorway (+Z in the model) towards the fire.
        h.transform().lookAt({camp.x, hy, camp.y});
        h.transform().rotate({0, 1, 0}, glm::pi<float>());

        // A torch beside each door.
        const glm::vec2 tp = p + glm::normalize(camp - p) * 3.4f + glm::vec2(-std::sin(a), std::cos(a)) * 1.4f;
        auto& torch = scene.spawn<Torch>(props.material);
        torch.transform().setPosition({tp.x, terrain.height(tp.x, tp.y), tp.y});
    }

    // --- Crates: a pyramid of dynamic boxes to knock over -----------------------------
    auto crateMat = r.createStandardMaterial();
    crateMat->setAlbedoTexture(assets.loadTexture("textures/thatch.png")).setBaseColor({0.75f, 0.55f, 0.38f, 1.0f}).setRoughness(0.8f);
    auto crateMesh = ve::Mesh::create(r, ve::primitives::cube(glm::vec3(0.8f)), {}, "crate");
    const ve::Collider crateShape = ve::Collider::box(glm::vec3(0.4f)); // one shape shared by all crates
    const glm::vec2 cratesAt = camp + glm::vec2(4.0f, -9.0f);
    const float groundY = terrain.height(cratesAt.x, cratesAt.y);
    for (int row = 0; row < 4; ++row) {
        for (int i = 0; i < 4 - row; ++i) {
            auto& crate = scene.spawn("Crate");
            crate.tag = "crate";
            crate.addComponent<ve::MeshRenderer>(crateMesh, crateMat);
            const float x = (static_cast<float>(i) - (3.0f - static_cast<float>(row)) * 0.5f) * 0.82f;
            crate.transform().setPosition({cratesAt.x + x, groundY + 0.41f + static_cast<float>(row) * 0.81f, cratesAt.y});
            crate.addComponent<ve::Rigidbody>(crateShape, ve::BodyType::Dynamic).mass = 15.0f;
        }
    }

    // --- Reed screens: alpha-tested (cutout) material; their shadows have holes too -----
    auto reedMat = r.createStandardMaterial(ve::StandardMaterial::Mode::Cutout);
    reedMat->setAlbedoTexture(assets.loadTexture("textures/reed_screen.png"))
        .setUvScale({3.0f, 2.0f})
        .setAlphaCutoff(0.5f)
        .setRoughness(0.9f)
        .setVertexColorWeight(0.0f);
    auto reedMesh = ve::Mesh::create(r, ve::primitives::quad(2.6f, 1.9f), {}, "reed screen");
    for (int i = 0; i < 3; ++i) {
        const glm::vec2 p = camp + glm::vec2(1.0f - 2.7f * static_cast<float>(i), 7.5f + 0.4f * static_cast<float>(i));
        auto& screen = scene.spawn("Reed screen");
        screen.addComponent<ve::MeshRenderer>(reedMesh, reedMat);
        screen.transform().setPosition({p.x, terrain.height(p.x, p.y) - 0.05f, p.y});
        screen.transform().setLocalRotation(glm::angleAxis(glm::radians(-75.0f + 8.0f * static_cast<float>(i)), glm::vec3(0, 1, 0)));
        screen.addComponent<ve::Rigidbody>(ve::Collider::box({1.3f, 0.95f, 0.03f}).offset({0.0f, 0.95f, 0.0f}),
                                           ve::BodyType::Static);
    }

    // --- Player ---------------------------------------------------------------------------
    auto& player = scene.spawn<Player>(terrain);
    player.setThrowable(props.rocks.meshes[0], props.material, props.rocks.colliders[0]);
    const glm::vec2 start = camp + glm::vec2(9.0f, 5.0f);
    // Face west, towards the setting sun and the swamp.
    player.teleport({start.x, terrain.height(start.x, start.y), start.y}, glm::radians(78.0f));

    // --- Collectibles ---------------------------------------------------------------------
    auto stoneMat = r.createStandardMaterial();
    stoneMat->setBaseColor({1.0f, 0.6f, 0.25f, 1.0f}).setRoughness(0.3f).setEmissive({1.0f, 0.45f, 0.12f}, 6.0f);
    ve::MeshData stoneData = ve::primitives::icosphere(0.22f, 0);
    stoneData.makeFlatShaded();
    auto stoneMesh = ve::Mesh::create(r, stoneData, {}, "sunstone");
    auto pickup = assets.loadAudio("audio/pickup.wav");
    Rng rng(99);
    int placed = 0;
    for (int attempt = 0; attempt < 400 && placed < 12; ++attempt) {
        const glm::vec2 p = camp + glm::vec2(rng.range(-90.0f, 90.0f), rng.range(-90.0f, 90.0f));
        const float h = terrain.height(p.x, p.y);
        if (h < 0.5f || glm::length(p - camp) < 6.0f || terrain.slope(p.x, p.y) > 0.3f) continue;
        auto& stone = scene.spawn<Sunstone>(stoneMesh, stoneMat, player, pickup);
        stone.transform().setPosition({p.x, h, p.y});
        ++placed;
    }
    gameState().total = placed;

    // --- Ambience + HUD ------------------------------------------------------------------
    auto& ambience = scene.spawn("Ambience");
    auto& wind = ambience.addComponent<ve::AudioSource>(assets.loadAudio("audio/wind_loop.wav"));
    wind.loop = true;
    wind.spatial = false;
    wind.volume = 0.35f;
    wind.playOnStart = true;
    scene.spawn<Hud>(player, dayNight, weather);

    // Test hooks (handy for screenshots / automated checks):
    //   SAVANNA_TIME=21.5  SAVANNA_WEATHER=0..3  SAVANNA_DEBUG=1  SAVANNA_FLY=x,y,z,yawDeg,pitchDeg
    if (const char* t = std::getenv("SAVANNA_TIME")) dayNight.timeOfDay = std::strtof(t, nullptr);
    if (const char* w = std::getenv("SAVANNA_WEATHER")) {
        weather.setWeather(static_cast<vfx::Weather::Type>(std::atoi(w) % int(vfx::Weather::Type::Count)), 0.01f);
    }
    if (std::getenv("SAVANNA_DEBUG")) engine.ui().debugOverlay = true;
    if (std::getenv("SAVANNA_AUTOTEST")) scene.spawn<AutoTest>(terrain);
    if (const char* f = std::getenv("SAVANNA_POSE")) {
        float x, y, z, yaw, pitch;
        if (std::sscanf(f, "%f,%f,%f,%f,%f", &x, &y, &z, &yaw, &pitch) == 5) player.setPose({x, y, z}, yaw, pitch);
    }
}

} // namespace

int main()
{
    try {
        ve::EngineConfig config;
        config.title = "Savanna - vulkan-engine test scene";
        config.width = 1600;
        config.height = 900;
        config.renderer.maxShadowedDirectionalLights = 2; // sun + moon
        config.renderer.cascadeCount = 4;
        config.renderer.maxShadowedPointLights = 4;      // campfire + torches
        config.renderer.maxShadowDistance = 180.0f;
        if (const char* v = std::getenv("VE_VALIDATION")) config.validation = v[0] == '1';

        ve::Engine engine(config);
        buildWorld(engine);
        engine.run();
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
