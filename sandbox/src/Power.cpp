#include "Power.hpp"

#include <ve/gameplay/Placement.hpp>
#include <ve/scene/Components.hpp>
#include <ve/scene/Reflection.hpp>
#include <ve/scene/Scene.hpp>

#include <algorithm>
#include <functional>

namespace savanna {

VE_REGISTER_TYPE(Generator, "Generator")
{
    t.category("Savanna");
    t.field("powerRadius", &Generator::powerRadius).range(0.0f, 200.0f).tooltip("lamps within this (m) are powered");
    t.field("fuel", &Generator::fuel).range(0.0f, 1000.0f).tooltip("litres");
    t.field("maxFuel", &Generator::maxFuel).range(0.0f, 1000.0f);
    t.field("burnPerMinute", &Generator::burnPerMinute).range(0.0f, 100.0f).speed(0.01f);
}

VE_REGISTER_TYPE(PoweredLight, "PoweredLight")
{
    t.category("Savanna");
}

VE_REGISTER_TYPE(FuelBarrel, "FuelBarrel")
{
    t.category("Savanna");
    t.field("litres", &FuelBarrel::litres).range(0.0f, 1000.0f);
    t.field("reach", &FuelBarrel::reach).range(0.0f, 50.0f).tooltip("pours into a generator this close (m)");
}

float Generator::addFuel(float litres)
{
    const float accepted = std::clamp(maxFuel - fuel, 0.0f, std::max(litres, 0.0f));
    fuel += accepted;
    return accepted;
}

void Generator::update(float dt)
{
    if (ve::PlacementGhost::is(gameObject())) return;
    if (fuel > 0.0f) fuel = std::max(0.0f, fuel - burnPerMinute * dt / 60.0f);
}

Generator* Generator::covering(const ve::Scene& scene, const glm::vec3& p, bool needFuel)
{
    // Each generator has its own reach: the engine's nearest-of-type lookup,
    // filtered by that generator's own radius (and fuel), measured on the ground.
    return scene.findNearest<Generator>(
        p, std::numeric_limits<float>::infinity(),
        [&](Generator& g) {
            if (ve::PlacementGhost::is(g.gameObject()) || (needFuel && !g.running())) return false;
            return ve::Scene::distanceTo(p, ve::Scene::positionOf(g), true) <= g.powerRadius;
        },
        true);
}

void PoweredLight::update(float dt)
{
    m_sinceCheck += dt;
    if (m_sinceCheck < 0.25f) return;
    m_sinceCheck = 0.0f;
    const bool lit = !ve::PlacementGhost::is(gameObject()) && Generator::covering(gameObject().scene(), transform().position(), true) != nullptr;
    if (lit == m_lit) return;
    m_lit = lit;
    // Every light on the lamp (the bulb's glow, the spot pointing down).
    std::function<void(ve::GameObject&)> set = [&](ve::GameObject& o) {
        for (const auto& c : o.components())
            if (auto* l = dynamic_cast<ve::PointLight*>(c.get())) l->enabled = lit;
        for (ve::Transform* ch : o.transform().children()) set(ch->gameObject());
    };
    set(gameObject());
}

Generator* FuelBarrel::pour()
{
    Generator* g = gameObject().scene().findNearest<Generator>(transform().position(), reach, [](Generator& gen) {
        return !ve::PlacementGhost::is(gen.gameObject()) && gen.fuel < gen.maxFuel;
    });
    if (!g) return nullptr;
    g->addFuel(litres);
    gameObject().destroy();
    return g;
}

} // namespace savanna
