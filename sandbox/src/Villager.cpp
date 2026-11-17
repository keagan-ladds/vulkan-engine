#include "Villager.hpp"

#include <ve/anim/Animation.hpp>
#include <ve/assets/AssetManager.hpp>
#include <ve/assets/Model.hpp>
#include <ve/core/Log.hpp>
#include <ve/scene/Reflection.hpp>
#include <ve/scene/Scene.hpp>
#include <ve/world/Terrain.hpp>

#include <cmath>

namespace savanna {

VE_REGISTER_TYPE(Villager, "Villager")
{
    t.category("Savanna");
    t.field("model", &Villager::model).assets(".gltf .glb");
    t.field("wanderRadius", &Villager::wanderRadius).range(0.0f, 200.0f);
    t.field("speed", &Villager::speed).range(0.0f, 10.0f).speed(0.01f);
    t.field("restTime", &Villager::restTime).range(0.0f, 60.0f);
    t.field("seed", &Villager::seed);
}

void Villager::start()
{
    m_rng.seed(uint32_t(seed) * 7919u + 17u);
    m_home = transform().position();
    m_terrain = ve::Terrain::find(scene());
    try {
        auto m = assets().loadModel(model);
        m_body = &scene().instantiate(*m, this);
        m_animator = m_body->getComponent<ve::Animator>();
    } catch (const std::exception& e) {
        ve::log::warn("Villager: {}", e.what());
    }
    m_yaw = std::uniform_real_distribution<float>(0.0f, 6.2831853f)(m_rng);
    m_rest = restTime * 0.5f;
    if (m_animator) m_animator->play("Idle", 0.0f);
}

void Villager::pickTarget()
{
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    const float a = u(m_rng) * 6.2831853f, r = std::sqrt(u(m_rng)) * wanderRadius;
    m_target = glm::vec2(m_home.x, m_home.z) + glm::vec2(std::cos(a), std::sin(a)) * r;
}

void Villager::update(float dt)
{
    glm::vec3 p = transform().position();
    if (m_walking) {
        const glm::vec2 to = m_target - glm::vec2(p.x, p.z);
        const float dist = glm::length(to);
        if (dist < 0.2f) {
            m_walking = false;
            m_rest = restTime * std::uniform_real_distribution<float>(0.5f, 1.5f)(m_rng);
            if (m_animator) m_animator->play("Idle", 0.3f);
        } else {
            // Turn towards the target (the model faces +Z), then walk.
            const float want = std::atan2(to.x, to.y);
            float d = std::remainder(want - m_yaw, 6.2831853f);
            m_yaw += std::clamp(d, -4.0f * dt, 4.0f * dt);
            const float step = std::min(dist, speed * dt * std::max(0.0f, std::cos(d)));
            p.x += std::sin(m_yaw) * step;
            p.z += std::cos(m_yaw) * step;
        }
    } else if ((m_rest -= dt) <= 0.0f) {
        pickTarget();
        m_walking = true;
        if (m_animator) m_animator->play("Walk", 0.3f);
    }
    if (m_terrain) p.y = m_terrain->height(p.x, p.z);
    transform().setPosition(p);
    transform().setRotation(glm::angleAxis(m_yaw, glm::vec3(0, 1, 0)));
}

} // namespace savanna
