#pragma once

// =============================================================================
// Gameplay props for the test scene: campfire, torches, collectible
// "sunstones", the water plane, and the HUD.
// =============================================================================

#include <ve/scene/GameObject.hpp>
#include <ve/world/Foliage.hpp>

#include <memory>
#include <set>
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
    int wood = 0, berries = 0; // from chopped trees / picked bushes (Harvest.hpp)
    float messageTimer = 0.0f;
    std::string message;
};
GameState& gameState();

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
class AutoTest : public ve::GameObject {
public:
    explicit AutoTest(const ve::Terrain& terrain) : GameObject("AutoTest"), m_terrain(&terrain) {}
    void update(float dt) override;

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
    float m_t = 0.0f;
    int m_step = 0;
};

class Hud : public ve::GameObject {
public:
    explicit Hud(Player& player);
    void update(float dt) override;
    void onGui() override;

private:
    void findEnvironment(); // the scene's Weather / DayNightCycle (scene objects)
    Player& m_player;
    vfx::DayNightCycle* m_dayNight = nullptr; // looked up by id every frame (the editor can delete them)
    vfx::Weather* m_weather = nullptr;
    uint64_t m_dayNightId = 0, m_weatherId = 0;
    bool m_showHelp = true;
    bool m_combat = false; // M: stand-in for a fight (pushes the "Combat" music mood)
};

} // namespace savanna
