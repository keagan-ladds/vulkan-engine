#pragma once

// =============================================================================
// Gameplay props for the test scene: campfire, torches, collectible
// "sunstones", the water plane, and the HUD.
// =============================================================================

#include <ve/scene/GameObject.hpp>
#include <ve/scene/Reflection.hpp>
#include <ve/ui/Gui.hpp>
#include <ve/world/Foliage.hpp>

#include "Inventory.hpp"

#include <memory>
#include <set>
#include <functional>
#include <string>
#include <vector>

namespace ve {
class Terrain;
class Material;
class Mesh;
class PointLight;
class AudioClip;
} // namespace ve
namespace vfx {
class Fire;
class ParticleEmitter;
class Weather;
class DayNightCycle;
} // namespace vfx

namespace savanna {

class Player;

// Shared game state (a real game would have an inventory system).
struct GameState {
    int collected = 0;
    int total = 0;
    Inventory inventory; // wood, berries, sunstones... (Inventory.hpp)
    float messageTimer = 0.0f;
    std::string message;
};
GameState& gameState();

// Save games (F5 / F9): the engine captures the level's changes and every
// ve::Saveable (player, regrowth, foliage, terrain, time of day); the
// counters above go in the save's "game" section.
bool saveProgress(ve::Scene& scene, const std::string& slot);
// Loads by rebuilding the level behind the loading screen (World.hpp), then applying the save.
bool loadProgress(const std::string& slot);
void applyGameData(const ve::Json& game); // the counters, from a save's "game" section
// Control bindings (F10 rebinds them): loaded over the defaults, saved next to the save games.
void loadControls();
void saveControls();

// The shared vertex-coloured prop material (set once the prop library exists;
// a plain one is created on first use otherwise, e.g. in another game).
void setPropMaterial(std::shared_ptr<ve::Material> material);
std::shared_ptr<ve::Material> propMaterial();

// All of the classes below are registered for the editor / scene files
// (VE_REGISTER_TYPE in Props.cpp) and default constructible.

// Fire pit: flickering shadow-casting point light, flames, smoke, sparks and
// a looping 3D crackle sound.
class Campfire : public ve::GameObject {
public:
    Campfire() : GameObject("Campfire") {}
    void start() override;
    void update(float dt) override;
    float baseIntensity = 22.0f;

private:
    std::shared_ptr<ve::Material> m_material;
    ve::PointLight* m_light = nullptr;
    vfx::Fire* m_fire = nullptr;
    float m_t = 0.0f;
};

// Wooden pole with a small flame and a shadow-casting point light.
class Torch : public ve::GameObject {
public:
    Torch() : GameObject("Torch") {}
    float intensity = 7.0f;
    void start() override;
    void update(float dt) override;

private:
    std::shared_ptr<ve::Material> m_material;
    ve::PointLight* m_light = nullptr;
    vfx::Fire* m_fire = nullptr;
    float m_t = 0.0f;
};

// Glowing stone that bobs and spins; press E near it to pick it up.
class Sunstone : public ve::GameObject {
public:
    Sunstone() : GameObject("Sunstone") { tag = "collectible"; }
    void start() override;
    void update(float dt) override;
    void collect();
    static int remaining(const ve::Scene& scene); // uncollected stones in the scene

private:
    Player* m_player = nullptr; // found by name ("Player") on start
    std::shared_ptr<ve::AudioClip> m_sound;
    ve::GameObject* m_visual = nullptr;
    glm::vec3 m_base{0.0f};
    float m_t = 0.0f;
};

// Woven reed fence panel: alpha-tested (cutout) material whose shadow has
// holes too, plus a thin static collider.
class ReedScreen : public ve::GameObject {
public:
    ReedScreen() : GameObject("Reed screen") {}
    void start() override;
};

// Where the player starts (Savanna spawns the player here after loading the scene).
class PlayerStart : public ve::GameObject {
public:
    PlayerStart() : GameObject("PlayerStart") {}
};


// Scripted smoke test (SAVANNA_AUTOTEST=1): picks up a sunstone, cycles the
// weather, then quits, logging what happened. Exercises gameplay code paths
// (deferred destroy, one-shot audio, weather transitions) without input.
// What the autotest saved (checked after the reload).
struct AutoTestSaveExpect {
    int collected = 0, wood = 0, berries = 0, stones = 0;
    glm::vec3 player{0.0f};
    uint64_t tree = 0;
    std::vector<std::pair<std::string, glm::vec3>> pieces; // building test: name, position
};

class AutoTest : public ve::GameObject {
public:
    static void startAt(int step) { if (s_resume == 0) s_resume = step; } // (a reload resumes where it was)
    explicit AutoTest(const ve::Terrain& terrain) : GameObject("AutoTest"), m_terrain(&terrain), m_step(s_resume) { s_resume = 0; }
    void update(float dt) override;

    // Loading a save rebuilds the level (and this test object): the new one resumes here.
    static inline int s_resume = 0;
    static inline AutoTestSaveExpect s_expect;
    // Building test helpers.
    void buildStep(int piece, const glm::vec3& aim, float yawDegrees);
    int foliageAt(const glm::vec3& p, float radius) const;
    int m_foliageOnSite = 0, m_foliageDuring = 0;
    glm::vec3 m_exclusionAt{0.0f};
    uint64_t m_exclusionObject = 0;
    uint32_t m_frames = 0, m_stepFrame = 0; // frames: steps that need the preview to settle
    glm::vec3 m_site{0.0f};
    std::vector<uint64_t> m_built;
    bool m_buildShot = false;
    void uiSteps(const std::function<void(const std::string&)>& log);

private:
    glm::vec3 groundAt(glm::vec2 p) const;
    void shot(const char* name);
    std::set<std::string> m_shotTaken;
    const ve::Terrain* m_terrain;
    ve::FoliageInstanceId m_tree, m_bush; // interactive foliage checks
    std::vector<glm::vec3> m_crateStart;
    bool m_throwPending = false;
    bool m_inTrigger = false;
    glm::vec3 m_triggerCenter{0.0f}, m_triggerFeet{0.0f};
    ve::GameObject* m_destroyProbe = nullptr;
    glm::vec3 m_padStart{0.0f};
    float m_followStart = 0.0f;
    uint64_t m_follower = 0;
    float m_t = 0.0f;
    int m_step;
};

class Hud : public ve::GameObject {
public:
    explicit Hud(Player& player);
    void start() override;
    void update(float dt) override;
    void onGui() override;
    void setVisible(bool visible); // (the main menu hides it)
    void setShowHelp(bool show) { m_showHelp = show; }

private:
    void findEnvironment(); // the scene's Weather / DayNightCycle (scene objects)
    Player& m_player;
    vfx::DayNightCycle* m_dayNight = nullptr; // looked up by id every frame (the editor can delete them)
    vfx::Weather* m_weather = nullptr;
    uint64_t m_dayNightId = 0, m_weatherId = 0;
    void drawControls(); // F10: actions, their bindings, rebinding
    bool m_showHelp = true;
    bool m_showControls = false;
    bool m_controlsShotDone = false;
    bool m_wasRebinding = false;
    bool m_combat = false; // M: stand-in for a fight (pushes the "Combat" music mood)
    ve::ui::Canvas* m_canvas = nullptr; // the HUD (ve::ui elements, see start())
    bool m_wanted = true;
};

} // namespace savanna
