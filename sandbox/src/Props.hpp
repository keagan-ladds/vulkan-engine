#pragma once

// =============================================================================
// Gameplay props for the test scene: campfire, torches, collectible
// "sunstones", the water plane, and the HUD.
// =============================================================================

#include <ve/scene/GameObject.hpp>
#include <ve/scene/ObjectRef.hpp>
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
// A short message in the HUD's prompt pill ("Game saved", "Sunstone collected!").
void notify(const std::string& text, float seconds = 2.5f);

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
    void onDestroy() override;
    float baseIntensity = 22.0f;
    // Every started campfire (standing in one hurts: Player), so nobody has
    // to search the scene for them each frame. Resolve each with get(scene).
    static const std::vector<ve::ObjectRef<Campfire>>& all();

private:
    std::shared_ptr<ve::Material> m_material;
    // On child objects the editor can delete.
    ve::ComponentRef<ve::PointLight> m_light;
    ve::ComponentRef<vfx::Fire> m_fire;
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
    // On child objects the editor can delete.
    ve::ComponentRef<ve::PointLight> m_light;
    ve::ComponentRef<vfx::Fire> m_fire;
};

// Glowing stone that bobs and spins; press E near it to pick it up.
class Sunstone : public ve::GameObject {
public:
    Sunstone() : GameObject("Sunstone") { tag = "collectible"; }
    void start() override;
    void update(float dt) override;
    void onDestroy() override;
    void collect();
    static int remaining(const ve::Scene& scene); // uncollected stones in the scene

private:
    std::shared_ptr<ve::AudioClip> m_sound;
    ve::ObjectRef<> m_visual; // a child: the editor can delete it
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


} // namespace savanna
