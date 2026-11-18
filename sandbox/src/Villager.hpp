#pragma once

// A villager (skinned glTF model): game-side AI on top of the engine's
// navigation. The engine's ve::NavAgent walks paths around huts, trees and
// other villagers; this class decides where to go:
//
//   Idle    rests for a while (Idle clip)
//   Wander  walks to a random reachable spot near home (Walk clip)
//   Follow  "Walk with me" (Use / E): keeps near the player for a while,
//           jogging to catch up, then walks home
//
// Placed in scenes/savanna.json; the level's NavMeshSurface covers the camp.

#include <ve/scene/GameObject.hpp>

#include <glm/glm.hpp>

#include <random>
#include <string>

namespace ve {
class Animator;
class NavAgent;
} // namespace ve

namespace savanna {

class Villager : public ve::GameObject {
public:
    enum class Mode { Idle, Wander, Follow, Return };

    Villager();
    void start() override;
    void update(float dt) override;

    std::string model = "models/villager.gltf";
    float wanderRadius = 8.0f; // around the start position
    float speed = 1.3f;        // walking, m/s (jogging when following: x2)
    float restTime = 3.0f;     // seconds at each spot (randomised +-50 %)
    float followTime = 30.0f;  // seconds of "walk with me"
    int seed = 1;

    Mode mode() const { return m_mode; }
    bool walking() const { return m_walking; }
    ve::Animator* animator() const { return m_animator; }
    ve::NavAgent& agent() const { return *m_agent; }
    void follow(ve::GameObject& who); // starts following (as if the player asked)
    void goHome();
    void wave(float seconds = 2.4f);  // raises the right arm (an animation layer over Idle / Walk)
    int steps() const { return m_steps; } // footsteps so far (Walk clip events)
    ve::GameObject* body() const { return m_body; }

private:
    void setMode(Mode m);
    void pickWanderTarget();

    ve::GameObject* m_body = nullptr; // the model instance (child)
    ve::Animator* m_animator = nullptr;
    ve::NavAgent* m_agent = nullptr;
    glm::vec3 m_home{0.0f};
    Mode m_mode = Mode::Idle;
    float m_timer = 0.0f;    // rest / follow time left
    float m_repath = 0.0f;   // follow: seconds to the next destination update
    uint64_t m_leader = 0;   // who we follow (object id)
    bool m_walking = false;
    std::mt19937 m_rng;
    int m_waveLayer = -1;
    float m_waveTimer = 0.0f;
    bool m_greeted = false; // waved at the player who came close (again once they've left)
    int m_steps = 0;
    void footstep();
};

} // namespace savanna
