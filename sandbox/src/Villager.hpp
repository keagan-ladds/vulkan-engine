#pragma once

// A villager (skinned glTF model, F2 animation test): wanders between random
// spots around where it was placed, walking with the "Walk" clip and resting
// with "Idle" (crossfaded). Placed in scenes/savanna.json.

#include <ve/scene/GameObject.hpp>

#include <glm/glm.hpp>

#include <random>
#include <string>

namespace ve {
class Animator;
class Terrain;
} // namespace ve

namespace savanna {

class Villager : public ve::GameObject {
public:
    Villager() : GameObject("Villager") {}
    void start() override;
    void update(float dt) override;

    std::string model = "models/villager.gltf";
    float wanderRadius = 8.0f; // around the start position
    float speed = 1.3f;        // m/s
    float restTime = 3.0f;     // seconds at each spot (randomised +-50 %)
    int seed = 1;

    bool walking() const { return m_walking; }
    ve::Animator* animator() const { return m_animator; }

private:
    void pickTarget();

    ve::GameObject* m_body = nullptr; // the model instance (child)
    ve::Animator* m_animator = nullptr;
    ve::Terrain* m_terrain = nullptr;
    glm::vec3 m_home{0.0f};
    glm::vec2 m_target{0.0f};
    float m_rest = 0.0f;
    float m_yaw = 0.0f;
    bool m_walking = false;
    std::mt19937 m_rng;
};

} // namespace savanna
