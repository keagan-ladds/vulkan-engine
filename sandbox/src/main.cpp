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
#include "Player.hpp"
#include "Harvest.hpp"
#include "Props.hpp"
#include "SavannaLayout.hpp"
#include "Vegetation.hpp"

#include <ve/editor/Editor.hpp>
#include <ve/world/Terrain.hpp>

#include <random>
#include <ve/scene/MeshComponents.hpp>
#include <ve/scene/SceneSerializer.hpp>
#include <ve/ve.hpp>
#include <vfx/Atmosphere.hpp>
#include <vfx/Bloom.hpp>
#include <vfx/ColorGrading.hpp>
#include <vfx/DepthOfField.hpp>
#include <vfx/PhotoMode.hpp>
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
// Test hook: five grass benders placed in front of the camera once it exists.
class BenderRing : public ve::GameObject {
public:
    BenderRing(ve::Terrain* terrain, float ahead, float side) : m_terrain(terrain), m_ahead(ahead), m_side(side) {}
    void update(float) override
    {
        ve::Camera* cam = scene().mainCamera();
        if (m_placed || !cam) return;
        m_placed = true;
        const glm::vec3 eye = cam->transform().position();
        glm::vec3 f = cam->transform().forward();
        f = glm::normalize(glm::vec3(f.x, 0.0f, f.z));
        const glm::vec3 r(-f.z, 0.0f, f.x);
        const glm::vec3 c = eye + f * m_ahead + r * m_side;
        for (int i = 0; i < 5; ++i) {
            const float a = float(i) * 1.2566f, x = c.x + std::cos(a) * 3.0f, z = c.z + std::sin(a) * 3.0f;
            auto& o = scene().spawn("TestBender");
            o.transform().setLocalPosition({x, m_terrain->height(x, z), z});
            o.addComponent<ve::GrassBender>().radius = 0.4f + 0.3f * float(i);
        }
    }

private:
    ve::Terrain* m_terrain;
    float m_ahead, m_side;
    bool m_placed = false;
};

// Test hook: a striped box sliding left/right in front of the camera (TAA motion vectors).
class Mover : public ve::GameObject {
public:
    void update(float dt) override
    {
        ve::Camera* cam = scene().mainCamera();
        if (!cam) return;
        if (!m_started) {
            m_started = true;
            const glm::vec3 f = glm::normalize(glm::vec3(cam->transform().forward().x, 0.0f, cam->transform().forward().z));
            m_right = glm::vec3(-f.z, 0.0f, f.x);
            m_centre = cam->transform().position() + f * 5.0f - glm::vec3(0.0f, 0.4f, 0.0f);
            auto& box = addComponent<ve::PrimitiveRenderer>();
            box.size = {1.0f, 1.0f, 1.0f};
            box.color = {0.9f, 0.9f, 0.9f, 1.0f};
            box.texture = "textures/reed_screen.png"; // high-contrast detail to judge sharpness
        }
        m_t += dt;
        transform().setLocalPosition(m_centre + m_right * (std::sin(m_t * 1.5f) * 2.0f));
    }

private:
    glm::vec3 m_centre{0.0f}, m_right{1.0f, 0.0f, 0.0f};
    float m_t = 0.0f;
    bool m_started = false;
};


// SAVANNA_PHOTO test hook: enters photo mode after the world settled, takes a
// 1-sample normal-quality reference shot and then the real one (same frozen
// view), then quits.
class PhotoShot : public ve::GameObject {
public:
    PhotoShot(vfx::PhotoMode& pm, int samples, int resolution, float aperture)
        : GameObject("PhotoShot"), m_pm(pm), m_samples(samples), m_resolution(resolution), m_aperture(aperture)
    {
        runInEditor = true;
    }
    void update(float) override
    {
        m_t += time().unscaledDeltaTime;
        if (m_state == 0 && m_t > 4.0f) {
            m_pm.enter();
            // A fixed view of the camp (the headless player's view isn't stable).
            const glm::vec3 eye(layout::kCamp.x - 8.0f, 4.8f, layout::kCamp.y + 12.0f);
            const glm::vec3 d = glm::vec3(layout::kCamp.x, 3.6f, layout::kCamp.y) - eye;
            m_pm.setView(eye, glm::degrees(std::atan2(-d.x, -d.z)), glm::degrees(std::asin(d.y / glm::length(d))));
            if (m_aperture > 0.0f)
                if (auto* dof = renderer().graph().findPass<vfx::DepthOfFieldPass>()) {
                    dof->enabled = true;
                    dof->aperture = m_aperture;
                }
            m_pm.samples = 1;
            m_pm.resolution = 0;
            m_pm.maxQuality = false;
            m_pm.takePhoto();
            m_state = 1;
        } else if (m_state == 1 && !m_pm.lastPhoto().empty()) {
            m_reference = m_pm.lastPhoto();
            m_pm.samples = m_samples;
            m_pm.resolution = m_resolution;
            m_pm.maxQuality = true;
            m_t = 0.0f;
            m_state = 2;
        } else if (m_state == 2 && m_t > 1.2f) { // file names have 1 s resolution
            m_pm.takePhoto();
            m_state = 3;
        } else if (m_state == 3 && m_pm.lastPhoto() != m_reference) {
            m_pm.exit();
            engine().quit();
            m_state = 4;
        }
    }

private:
    vfx::PhotoMode& m_pm;
    int m_samples, m_resolution;
    float m_aperture;
    std::string m_reference;
    float m_t = 0.0f;
    int m_state = 0;
};

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
    // and the player start, plus the vegetation: a Foliage object with one
    // FoliageType per prop (meshes from the MeshLibrary, registered first),
    // GrassLayers for grass and reeds, FoliageExclusions around the camp and huts.
    registerPropMeshes();
    const char* sceneFile = std::getenv("SAVANNA_SCENE"); // another scene file (tests)
    std::vector<ve::GameObject*> level = ve::SceneSerializer::load(scene, sceneFile ? sceneFile : "scenes/savanna.json");
    ve::Terrain* terrainPtr = ve::Terrain::find(scene);
    for (ve::GameObject* o : level) {
        if (!terrainPtr) terrainPtr = dynamic_cast<ve::Terrain*>(o);
    }
    if (!terrainPtr) terrainPtr = &scene.spawn<ve::Terrain>(); // a scene without terrain: default one
    ve::Terrain& terrain = *terrainPtr;

    // --- Player: spawns at the PlayerStart marker ------------------------------------------
    auto& player = scene.spawn<Player>();
    if (auto rocks = ve::MeshLibrary::get(r, "rock")) // throwable stones look like the scattered rocks
        player.setThrowable(rocks->variants[0][0].mesh, propMaterial(), rocks->colliders[0]);
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
    scene.spawn<Regrowth>(); // berry bushes grow back (Harvest.hpp)
    auto& photoMode = scene.spawn<vfx::PhotoMode>(); // F12: free camera, DOF, max-quality screenshots

    // Environment objects from the scene file (Weather, DayNightCycle, Water).
    vfx::Weather* weather = nullptr;
    vfx::DayNightCycle* dayNight = nullptr;
    for (ve::GameObject* o : level) {
        if (!weather) weather = dynamic_cast<vfx::Weather*>(o);
        if (!dayNight) dayNight = dynamic_cast<vfx::DayNightCycle*>(o);
    }

    // Test hooks (handy for screenshots / automated checks):
    //   SAVANNA_TIME=21.5  SAVANNA_WEATHER=0..4 (4 = thunderstorm)  SAVANNA_DEBUG=1  SAVANNA_POSE=x,y,z,yawDeg,pitchDeg
    if (const char* t = std::getenv("SAVANNA_TIME"); t && dayNight) dayNight->timeOfDay = std::strtof(t, nullptr);
    if (const char* w = std::getenv("SAVANNA_WEATHER"); w && weather) {
        weather->setWeather(static_cast<vfx::Weather::Type>(std::atoi(w) % int(vfx::Weather::Type::Count)), 0.01f);
    }
    if (std::getenv("SAVANNA_DEBUG")) engine.ui().debugOverlay = true;
    if (const char* b = std::getenv("SAVANNA_LODBIAS")) r.settings().lodBias = std::strtof(b, nullptr); // <1 = coarser
    r.settings().temporalAA = true;
    if (const char* o = std::getenv("SAVANNA_OCCLUSION")) r.settings().occlusionCulling = o[0] == '1'; // occlusion culling on/off
    // SAVANNA_MOVER=1: a textured box sliding sideways 5 m in front of the camera.
    // SAVANNA_VELOCITY=0 turns the motion vector pass off (TAA uses camera motion only).
    if (std::getenv("SAVANNA_MOVER")) scene.spawn<Mover>();
    if (const char* v = std::getenv("SAVANNA_VELOCITY"); v && v[0] == '0') {
        if (auto* p = r.graph().findPass<ve::VelocityPass>()) p->enabled = false;
    }
    // SAVANNA_BENDERS=ahead,side: a ring of grass benders (radius 0.4..1.6 m) on the
    // ground `ahead` m in front of the camera and `side` m to its right (screenshots).
    if (const char* b = std::getenv("SAVANNA_BENDERS")) {
        float ahead = 8.0f, side = 0.0f;
        std::sscanf(b, "%f,%f", &ahead, &side);
        scene.spawn<BenderRing>(terrainPtr, ahead, side);
    }
    if (const char* s = std::getenv("SAVANNA_SUNSTEP")) r.settings().shadowDirectionStep = std::strtof(s, nullptr); // degrees, 0 = continuous
    // SAVANNA_LIGHTS=<n>[,0]: n small coloured point lights scattered around the camp
    // (clustered lighting test; ",0" turns the clusters off for comparison).
    if (const char* l = std::getenv("SAVANNA_LIGHTS")) {
        int count = 0, clustered = 1;
        std::sscanf(l, "%d,%d", &count, &clustered);
        r.settings().clusteredLights = clustered != 0;
        std::mt19937 rng(7);
        std::uniform_real_distribution<float> u(0.0f, 1.0f);
        for (int i = 0; i < count; ++i) {
            const float x = (u(rng) - 0.5f) * 120.0f, z = (u(rng) - 0.5f) * 120.0f;
            auto& o = scene.spawn("TestLight");
            o.transform().setLocalPosition({x, (terrainPtr ? terrainPtr->height(x, z) : 0.0f) + 1.0f, z});
            auto& pl = o.addComponent<ve::PointLight>();
            pl.color = glm::vec3(u(rng), u(rng), u(rng)) * 0.8f + 0.2f;
            pl.intensity = 6.0f;
            pl.range = 6.0f + u(rng) * 6.0f;
        }
    }
    if (const char* a = std::getenv("SAVANNA_TAA")) r.settings().temporalAA = a[0] == '1'; // SAVANNA_TAA=0 turns it off
    if (std::getenv("SAVANNA_AUTOTEST")) scene.spawn<AutoTest>(terrain);
    // SAVANNA_PHOTO=<samples>[,<resolution 0..4>[,<dof aperture>]]: take a photo-mode shot of the start view and quit.
    if (const char* p = std::getenv("SAVANNA_PHOTO")) {
        int samples = 32, res = 0;
        float aperture = 0.0f;
        std::sscanf(p, "%d,%d,%f", &samples, &res, &aperture);
        scene.spawn<PhotoShot>(photoMode, samples, res, aperture);
    }
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
        // ~150k foliage instances in the 4 km world, and the editor briefly
        // holds the old and the new set while it reopens a scene.
        config.renderer.maxInstances = 1u << 19;
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
