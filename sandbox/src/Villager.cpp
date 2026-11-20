#include "Villager.hpp"

#include <ve/debug/Debug.hpp>
#include <ve/world/Terrain.hpp>
#include <ve/audio/Audio.hpp>

#include "Footsteps.hpp"
#include "Player.hpp"
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
            notify("\"I'll head back to the camp.\"", 3.0f);
        } else {
            follow(user);
            notify("\"Lead the way!\"", 3.0f);
        }
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
        if (m_animator) {
            // The right arm on its own layer: wave over whatever the body does.
            m_waveLayer = m_animator->addLayer("Wave", "UpperArm.R");
            m_animator->setLayerWeight(m_waveLayer, 0.0f, 0.0f);
            // Footsteps: the Walk clip's foot contacts (its legs swing on a 1 s cycle).
            m_animator->addEvent("Walk", 0.25f, "step");
            m_animator->addEvent("Walk", 0.75f, "step");
            m_animator->onEvent = [this](const std::string& e, const std::string&) {
                if (e == "step") footstep();
            };
        }
        // Feet on the ground on slopes and steps (two-bone IK per leg, hips lowered).
        auto& feet = m_body->addComponent<ve::FootIK>();
        feet.layers = ~(1u << std::max(0, physics().layer("Debris"))); // (not on loose rocks)
        // Head and eyes follow the player nearby; the right arm points things out.
        m_look = &m_body->addComponent<ve::LookAtIK>();
        m_point = &m_body->addComponent<ve::AimIK>();
        m_point->chain = {{"Chest", 0.15f}, {"UpperArm.R", 0.85f}};
        m_point->effector = "UpperArm.R";
        m_point->axis = {0.0f, -1.0f, 0.0f}; // the arm hangs down in the rest pose
        m_point->maxAngle = 170.0f;
        m_point->blendTime = 0.45f;
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
    wave(1.6f); // "sure, let's go"
    m_leader = who.id();
    m_timer = followTime;
    m_repath = 0.0f;
    setMode(Mode::Follow);
}

void Villager::wave(float seconds)
{
    if (!m_animator || m_waveLayer < 0) return;
    m_animator->play(m_waveLayer, "Wave", 0.0f);
    m_animator->setLayerWeight(m_waveLayer, 1.0f, 0.35f);
    m_waveTimer = seconds;
}

void Villager::pointAt(const glm::vec3& at, float seconds)
{
    if (!m_point) return;
    if (m_animator && m_waveLayer >= 0) m_animator->setLayerWeight(m_waveLayer, 0.0f, 0.2f); // (one arm, one job)
    m_waveTimer = 0.0f;
    m_point->setTarget(at);
    m_pointTimer = seconds;
}

void Villager::footstep()
{
    ++m_steps;
    // What's underfoot (terrain: its biome), quieter than the player's own steps.
    const glm::vec3 p = transform().position();
    const auto hit = physics().raycast(p + glm::vec3(0, 0.5f, 0), {0, -1, 0}, 1.5f);
    const std::string sound = footstepSound(scene(), hit ? hit->surface : std::string(), p); // (Footsteps.hpp, as the player's)
    try {
        auto clip = assets().loadAudio("audio/footstep_" + sound + "_" + std::to_string(m_steps % 3) + ".wav");
        ve::OneShotSettings s;
        s.volume = 0.22f;
        s.pitch = 0.95f + 0.1f * float(m_steps % 5) / 4.0f;
        s.position = p;
        s.maxDistance = 18.0f;
        audio().playOneShot(*clip, s);
    } catch (const std::exception&) {
    }
}

void Villager::goHome()
{
    m_leader = 0;
    setMode(Mode::Return);
    m_agent->setDestination(m_home);
}

void Villager::update(float dt)
{
    // The wave fades out when it's done; a player walking up gets one too,
    // then a hint: the arm points at the nearest sunstone.
    if (m_waveTimer > 0.0f && (m_waveTimer -= dt) <= 0.0f && m_animator) {
        m_animator->setLayerWeight(m_waveLayer, 0.0f, 0.4f);
        if (m_hintAfterWave) {
            m_hintAfterWave = false;
            const glm::vec3 me = transform().position();
            const ve::GameObject* best = nullptr;
            float bestD = 60.0f;
            for (const auto& o : scene().objects())
                if (dynamic_cast<const Sunstone*>(o.get()) && !o->isDestroyed())
                    if (const float d = glm::distance(o->transform().position(), me); d < bestD) {
                        bestD = d;
                        best = o.get();
                    }
            if (best) pointAt(best->transform().position());
        }
    }
    if (m_pointTimer > 0.0f && (m_pointTimer -= dt) <= 0.0f && m_point) m_point->clearTarget();
    if (const Player* player = localPlayer(scene())) {
        const glm::vec3 eyes = player->transform().position() + glm::vec3(0.0f, 1.6f, 0.0f);
        const float d = glm::distance(player->transform().position(), transform().position());
        if (d < 4.0f && !m_greeted && m_mode != Mode::Follow) {
            m_greeted = true;
            wave();
            m_hintAfterWave = true;
        } else if (d > 8.0f) {
            m_greeted = false;
        }
        if (m_look) { // watch the player who's close
            if (d < 8.0f) m_look->setTarget(eyes);
            else m_look->clearTarget();
        }
    }
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

    // F1 > Debug draw > "Villagers": what each one is doing (a game channel next to the engine's).
    if (ve::debug::enabled("Villagers")) {
        static const char* kModes[] = {"Idle", "Wander", "Follow", "Return"};
        ve::debug::text(transform().position() + glm::vec3(0, 2.2f, 0), std::string(name()) + ": " + kModes[int(m_mode)], ve::debug::kYellow);
    }
}

} // namespace savanna
