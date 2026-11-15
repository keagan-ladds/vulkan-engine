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
#include <vfx/ColorGrading.hpp>
#include <vfx/DepthOfField.hpp>
#include <vfx/Particles.hpp>
#include <vfx/ScreenSpaceGI.hpp>
#include <vfx/ScreenSpaceReflections.hpp>
#include <vfx/Vfx.hpp>
#include <vfx/VolumetricFog.hpp>
#include <vfx/Weather.hpp>

#include <glm/gtc/constants.hpp>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <string>

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
    // Optional screen-space passes (toggle each in the F1 render-pass list).
    // SAVANNA_SSFX=0 starts with SSGI/SSR off (SAVANNA_SSGI=0 / SAVANNA_SSR=0 for one); SAVANNA_DOF=1 starts with depth of field on.
    const bool ssfx = !std::getenv("SAVANNA_SSFX") || std::string(std::getenv("SAVANNA_SSFX")) != "0";
    auto envOff = [](const char* name) { return std::getenv(name) && std::string(std::getenv(name)) == "0"; };
    r.graph().addPass<vfx::ScreenSpaceGIPass>().enabled = ssfx && !envOff("SAVANNA_SSGI");
    r.graph().addPass<vfx::ScreenSpaceReflectionsPass>().enabled = ssfx && !envOff("SAVANNA_SSR");
    r.graph().addPass<vfx::DepthOfFieldPass>().enabled = std::getenv("SAVANNA_DOF") != nullptr;
    // Colour grading + tonemapping (off by default: the composite's plain ACES).
    // Turn it on in F1 > Render passes > Color grading and pick a preset, or SAVANNA_GRADE=1.
    auto& grading = r.graph().addPass<vfx::ColorGradingPass>();
    grading.enabled = std::getenv("SAVANNA_GRADE") && std::string(std::getenv("SAVANNA_GRADE")) == "1";

    // --- World ---------------------------------------------------------------------
    auto& terrain = scene.spawn<Terrain>();
    scene.spawn<Water>();
    const PropLibrary props = buildPropLibrary(r);
    auto& foliage = scatterProps(scene, terrain, props);
    addGrass(r, terrain);

    // --- Weather + day/night ------------------------------------------------------------
    auto& weather = scene.spawn<vfx::Weather>();
    weather.rainSound = assets.loadAudio("audio/rain_loop.wav");
    weather.stormSound = assets.loadAudio("audio/rain_thunder_loop.wav", /*stream*/ true); // 48 s: stream from disk
    weather.windSound = assets.loadAudio("audio/wind_loop.wav");                           // volume follows the wind
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
        ve::log::info("Hut {} at ({:.1f}, {:.1f}, {:.1f}){}", i, p.x, hy, p.y, i == 1 ? " (no door)" : "");
        if (i == 1) {
            // The middle hut has no door, so you can walk in (and hear the rain muffle).
            std::function<void(ve::Transform&)> removeDoor = [&](ve::Transform& t) {
                for (ve::Transform* child : t.children()) {
                    if (child->gameObject().name() == "Door") child->gameObject().destroy(); // also removes its collider
                    else removeDoor(*child);
                }
            };
            removeDoor(h.transform());
        }
        // Indoors: rain and wind are muffled while the listener is inside the walls.
        auto& zone = scene.spawn("HutZone");
        zone.transform().setPosition({p.x, hy, p.y});
        auto& az = zone.addComponent<ve::AudioZone>();
        az.shape = ve::AudioZone::Shape::Cylinder;
        az.radius = 2.4f; // walls are at 2.6 m
        az.height = 4.0f;
        az.effects = {{"Weather", 0.3f, 450.0f}, {"Ambient", 0.45f, 900.0f}};

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
    player.setThrowable(props.rocks.variants[0][0].mesh, props.material, props.rocks.colliders[0]);
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
    // Soft ambient music bed (wind, rain and storm loops are driven by the Weather object).
    // On the Ambient bus, so the huts' AudioZones muffle it a little indoors.
    auto& ambience = scene.spawn("Ambience");
    auto& bed = ambience.addComponent<ve::AudioSource>(assets.loadAudio("audio/ambient_01.wav", /*stream*/ true));
    bed.loop = true;
    bed.spatial = false;
    bed.bus = "Ambient";
    bed.volume = 0.15f;
    bed.playOnStart = true;
    scene.spawn<Hud>(player, dayNight, weather);

    // Test hooks (handy for screenshots / automated checks):
    //   SAVANNA_TIME=21.5  SAVANNA_WEATHER=0..4 (4 = thunderstorm)  SAVANNA_DEBUG=1  SAVANNA_FLY=x,y,z,yawDeg,pitchDeg
    if (const char* t = std::getenv("SAVANNA_TIME")) dayNight.timeOfDay = std::strtof(t, nullptr);
    if (const char* w = std::getenv("SAVANNA_WEATHER")) {
        weather.setWeather(static_cast<vfx::Weather::Type>(std::atoi(w) % int(vfx::Weather::Type::Count)), 0.01f);
    }
    if (std::getenv("SAVANNA_DEBUG")) engine.ui().debugOverlay = true;
    if (const char* b = std::getenv("SAVANNA_LODBIAS")) r.settings().lodBias = std::strtof(b, nullptr); // <1 = coarser
    if (std::getenv("SAVANNA_AUTOTEST")) scene.spawn<AutoTest>(terrain);
    if (const char* f = std::getenv("SAVANNA_POSE")) {
        float x, y, z, yaw, pitch;
        if (std::sscanf(f, "%f,%f,%f,%f,%f", &x, &y, &z, &yaw, &pitch) == 5) player.setPose({x, y, z}, yaw, pitch);
    }

    // Props stream around the camera; colliders around the player. Load the
    // start area up front so nothing pops in on the first frames.
    foliage.colliderFocus = &player;
    foliage.preload(player.transform().position());
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
