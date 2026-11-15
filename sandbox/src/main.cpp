// =============================================================================
// Savanna - the engine's test scene.
//
// Builds a small open world that exercises every engine feature:
//   terrain/grass/water with custom shaders, glTF import, textures from PNG,
//   sun + moon cascaded shadows, point light cube shadows (campfire, torches),
//   sky + volumetric clouds + fog + bloom (ve_effects), particles, weather,
//   3D audio, ImGui HUD, and a fixed-timestep player controller.
// =============================================================================

#include "EditorTest.hpp"
#include "Noise2D.hpp"
#include "Player.hpp"
#include "Props.hpp"
#include "SavannaLayout.hpp"
#include "Vegetation.hpp"

#include <ve/editor/Editor.hpp>
#include <ve/world/Terrain.hpp>
#include <ve/scene/MeshComponents.hpp>
#include <ve/scene/SceneSerializer.hpp>
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
#include <vfx/DayNight.hpp>
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

    // --- The level: scenes/savanna.json (edit it in the editor, F2) ------------------------
    // Terrain (shape, biomes, stamps), water, weather, day/night, the camp (huts +
    // audio zones, campfire, torches, crates, reed screens), sunstones, ambience
    // and the player start. Vegetation rules are still code (below) until Phase W4.
    std::vector<ve::GameObject*> level = ve::SceneSerializer::load(scene, "scenes/savanna.json");
    ve::Terrain* terrainPtr = ve::Terrain::find(scene);
    for (ve::GameObject* o : level) {
        if (!terrainPtr) terrainPtr = dynamic_cast<ve::Terrain*>(o);
    }
    if (!terrainPtr) terrainPtr = &scene.spawn<ve::Terrain>(); // a scene without terrain: default one
    ve::Terrain& terrain = *terrainPtr;

    const PropLibrary props = buildPropLibrary(r);
    setPropMaterial(props.material); // shared by Campfire / Torch (scene objects)
    auto& foliage = scatterProps(scene, terrain, props);
    std::vector<glm::vec2> clearings; // no grass inside the huts
    for (ve::GameObject* o : level) {
        if (o->tag == "hut") clearings.emplace_back(o->transform().position().x, o->transform().position().z);
    }
    addGrass(r, terrain, clearings);

    // --- Player: spawns at the PlayerStart marker ------------------------------------------
    auto& player = scene.spawn<Player>();
    player.setThrowable(props.rocks.variants[0][0].mesh, props.material, props.rocks.colliders[0]);
    glm::vec2 start = layout::kCamp + glm::vec2(9.0f, 5.0f);
    float startYaw = glm::radians(78.0f);
    for (ve::GameObject* o : level) {
        if (dynamic_cast<PlayerStart*>(o)) {
            const glm::quat q = o->transform().rotation();
            start = {o->transform().position().x, o->transform().position().z};
            startYaw = 2.0f * std::atan2(q.y, q.w); // rotation about Y
        }
    }
    player.teleport({start.x, terrain.height(start.x, start.y), start.y}, startYaw);

    scene.spawn<Hud>(player);

    // Environment objects from the scene file (Weather, DayNightCycle, Water).
    vfx::Weather* weather = nullptr;
    vfx::DayNightCycle* dayNight = nullptr;
    for (ve::GameObject* o : level) {
        if (!weather) weather = dynamic_cast<vfx::Weather*>(o);
        if (!dayNight) dayNight = dynamic_cast<vfx::DayNightCycle*>(o);
    }

    // Test hooks (handy for screenshots / automated checks):
    //   SAVANNA_TIME=21.5  SAVANNA_WEATHER=0..4 (4 = thunderstorm)  SAVANNA_DEBUG=1  SAVANNA_FLY=x,y,z,yawDeg,pitchDeg
    if (const char* t = std::getenv("SAVANNA_TIME"); t && dayNight) dayNight->timeOfDay = std::strtof(t, nullptr);
    if (const char* w = std::getenv("SAVANNA_WEATHER"); w && weather) {
        weather->setWeather(static_cast<vfx::Weather::Type>(std::atoi(w) % int(vfx::Weather::Type::Count)), 0.01f);
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
        // SAVANNA_EDITOR=1: start in the editor (same as pressing F2).
        if (std::getenv("SAVANNA_EDITOR") && engine.editor()) engine.editor()->setMode(ve::Editor::Mode::Edit);
        if (std::getenv("SAVANNA_EDITORTEST")) engine.scene().spawn<savanna::EditorTest>();
        engine.run();
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
