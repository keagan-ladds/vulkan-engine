#include "Villager.hpp"

#include "Props.hpp"

#include <ve/anim/Animation.hpp>
#include <ve/assets/AssetManager.hpp>
#include <ve/assets/Model.hpp>
#include <ve/core/Log.hpp>
#include <ve/gameplay/Interaction.hpp>
#include <ve/nav/Navigation.hpp>
#include <ve/physics/Physics.hpp>
#include <ve/scene/Reflection.hpp>
#include <ve/scene/Scene.hpp>

#include <cmath>

namespace savanna {

VE_REGISTER_TYPE(Villager, "Villager")
{
    t.category("Savanna");
    t.field("model", &Villager::model).assets(".gltf .glb");
    t.field("wanderRadius", &Villager::wanderRadius).range(0.0f, 200.0f);
    t.field("speed", &Villager::speed).range(0.0f, 10.0f).speed(0.01f);
    t.field("restTime", &Villager::restTime).range(0.0f, 60.0f);
    t.field("followTime", &Villager::followTime).range(0.0f, 600.0f);
    t.field("seed", &Villager::seed);
}

Villager::Villager() : GameObject("Villager")
{
    // Walking is the engine's job: paths, turning, avoiding each other.
    m_agent = &addComponent<ve::NavAgent>();
    m_agent->radius = 0.35f;
    m_agent->height = 1.8f;
    m_agent->angularSpeed = 300.0f;
    m_agent->stoppingDistance = 0.4f;

    // Something to bump into and look at (the Interactor's ray finds it).
    auto& body = addComponent<ve::Rigidbody>(ve::Collider{}, ve::BodyType::Kinematic);
    body.shape = ve::ColliderShape::Capsule;
    body.shapeSize = {0.6f, 1.75f, 0.6f};
    body.shapeOffset = {0.0f, 0.9f, 0.0f};

    auto& talk = addComponent<ve::Interactable>();
    talk.reach = 3.5f;
    talk.focusOffset = {0.0f, 1.4f, 0.0f};
    talk.promptFn = [this] { return m_mode == Mode::Follow ? std::string("Thank villager") : std::string("Ask villager to walk with you"); };
    talk.onUse = [this](ve::GameObject& user) {
        if (m_mode == Mode::Follow) {
            goHome();
            gameState().message = "\"I'll head back to the camp.\"";
        } else {
            follow(user);
            gameState().message = "\"Lead the way!\"";
        }
        gameState().messageTimer = 3.0f;
    };
}

void Villager::start()
{
    m_rng.seed(uint32_t(seed) * 7919u + 17u);
    m_home = transform().position();
    try {
        auto m = assets().loadModel(model);
        m_body = &scene().instantiate(*m, this);
        // The model faces +Z; the agent turns our -Z (engine forward) along the way.
        m_body->transform().setLocalRotation(glm::angleAxis(3.14159265f, glm::vec3(0, 1, 0)));
        m_animator = m_body->getComponent<ve::Animator>();
    } catch (const std::exception& e) {
        ve::log::warn("Villager: {}", e.what());
    }
    const float yaw = std::uniform_real_distribution<float>(0.0f, 6.2831853f)(m_rng);
    transform().setRotation(glm::angleAxis(yaw, glm::vec3(0, 1, 0)));
    m_agent->speed = speed;
    m_timer = restTime * 0.5f;
    if (m_animator) m_animator->play("Idle", 0.0f);
}

void Villager::setMode(Mode m)
{
    m_mode = m;
    m_agent->speed = m == Mode::Follow ? speed * 2.0f : speed;
    m_agent->stoppingDistance = m == Mode::Follow ? 2.0f : 0.4f;
}

void Villager::pickWanderTarget()
{
    ve::NavMeshSurface* nav = ve::NavMeshSurface::find(scene());
    if (!nav) return;
    if (auto p = nav->randomPointNear(m_home, wanderRadius)) m_agent->setDestination(*p);
}

void Villager::follow(ve::GameObject& who)
{
    m_leader = who.id();
    m_timer = followTime;
    m_repath = 0.0f;
    setMode(Mode::Follow);
}

void Villager::goHome()
{
    m_leader = 0;
    setMode(Mode::Return);
    m_agent->setDestination(m_home);
}

void Villager::update(float dt)
{
    switch (m_mode) {
    case Mode::Idle:
        if ((m_timer -= dt) <= 0.0f) {
            setMode(Mode::Wander);
            pickWanderTarget();
        }
        break;
    case Mode::Wander:
    case Mode::Return:
        if (m_agent->arrived()) {
            setMode(Mode::Idle);
            m_timer = restTime * std::uniform_real_distribution<float>(0.5f, 1.5f)(m_rng);
        }
        break;
    case Mode::Follow: {
        ve::GameObject* leader = scene().findById(m_leader);
        if (!leader || (m_timer -= dt) <= 0.0f) {
            goHome();
            break;
        }
        // The leader keeps moving: aim at where they are now, a few times a
        // second, but only once they're out of comfortable range (no creeping
        // closer every time the destination is refreshed).
        if ((m_repath -= dt) <= 0.0f) {
            const glm::vec3 to = leader->transform().position() - transform().position();
            const float d = glm::length(glm::vec2(to.x, to.z));
            if (d > 3.0f) m_agent->setDestination(leader->transform().position());
            else if (d < 2.2f && m_agent->hasDestination()) m_agent->stop();
            m_repath = 0.4f;
        }
        break;
    }
    }

    // Animation follows the actual speed (Idle / Walk, played faster when jogging).
    const float v = glm::length(glm::vec2(m_agent->velocity().x, m_agent->velocity().z));
    const bool walking = v > 0.25f;
    if (m_animator) {
        if (walking != m_walking) m_animator->play(walking ? "Walk" : "Idle", 0.3f);
        m_animator->speed = walking ? std::clamp(v / std::max(speed, 0.1f), 0.6f, 2.2f) : 1.0f;
    }
    m_walking = walking;
}

} // namespace savanna
