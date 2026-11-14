#pragma once

// =============================================================================
// Gameplay props for the test scene: campfire, torches, collectible
// "sunstones", the water plane, and the HUD.
// =============================================================================

#include <ve/scene/GameObject.hpp>

#include <memory>
#include <string>

namespace ve {
class Material;
class Mesh;
class PointLight;
class AudioClip;
} // namespace ve
namespace vfx {
class ParticleEmitter;
class Weather;
} // namespace vfx

namespace savanna {

class Player;
class DayNightCycle;

// Shared game state (a real game would have an inventory system).
struct GameState {
    int collected = 0;
    int total = 0;
    float messageTimer = 0.0f;
    std::string message;
};
GameState& gameState();

// Fire pit: flickering shadow-casting point light, flames, smoke, sparks and
// a looping 3D crackle sound.
class Campfire : public ve::GameObject {
public:
    explicit Campfire(std::shared_ptr<ve::Material> propMaterial);
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
    explicit Torch(std::shared_ptr<ve::Material> propMaterial);
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
    Sunstone(std::shared_ptr<ve::Mesh> mesh, std::shared_ptr<ve::Material> material, Player& player,
             std::shared_ptr<ve::AudioClip> pickupSound);
    void start() override;
    void update(float dt) override;
    void collect();

private:
    std::shared_ptr<ve::Mesh> m_mesh;
    std::shared_ptr<ve::Material> m_material;
    Player& m_player;
    std::shared_ptr<ve::AudioClip> m_sound;
    ve::GameObject* m_visual = nullptr;
    glm::vec3 m_base{0.0f};
    float m_t = 0.0f;
};

// Water plane with a custom transparent shader (soft shores, sky reflections, rain ripples).
class Water : public ve::GameObject {
public:
    Water();
};

// Scripted smoke test (SAVANNA_AUTOTEST=1): picks up a sunstone, cycles the
// weather, then quits, logging what happened. Exercises gameplay code paths
// (deferred destroy, one-shot audio, weather transitions) without input.
class AutoTest : public ve::GameObject {
public:
    AutoTest() : GameObject("AutoTest") {}
    void update(float dt) override;

private:
    float m_t = 0.0f;
    int m_step = 0;
};

class Hud : public ve::GameObject {
public:
    Hud(Player& player, DayNightCycle& dayNight, vfx::Weather& weather);
    void update(float dt) override;
    void onGui() override;

private:
    Player& m_player;
    DayNightCycle& m_dayNight;
    vfx::Weather& m_weather;
    bool m_showHelp = true;
};

} // namespace savanna
