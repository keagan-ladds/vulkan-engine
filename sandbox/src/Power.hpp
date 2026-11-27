#pragma once

// Power (game side): generators, the lamps they power and the fuel they burn,
// built on the engine's spatial lookups (ve::Scene::findNearest /
// forEachWithin): the engine finds things near a point, the rules are here.
//
//   Generator    a build piece's component: powers lamps within powerRadius
//                while it has fuel; burns fuel while running. Saved.
//   PoweredLight lights (PointLight / SpotLight on its object and children)
//                are on while a running generator covers them.
//   FuelBarrel   placed within `reach` of a generator, it pours its fuel in
//                and is gone (Builder calls pour() when it's placed).
//
// Building rules (Building.cpp): a lamp post can only be placed where a
// generator's powerRadius reaches ("needs a generator" in the build panel).

#include <ve/scene/GameObject.hpp>
#include <ve/scene/SaveGame.hpp>

#include <glm/glm.hpp>

namespace savanna {

class Generator : public ve::Component, public ve::Saveable {
public:
    float powerRadius = 16.0f;   // lamps within this (m, horizontal) are powered
    float fuel = 0.0f;           // litres in the tank
    float maxFuel = 60.0f;       // tank size (litres)
    float burnPerMinute = 0.5f;  // litres per minute while running

    bool running() const { return fuel > 0.0f; }
    // Pours up to `litres` in; returns how much fitted.
    float addFuel(float litres);
    void update(float dt) override;
    void saveState(ve::Json& out) const override { out = {{"fuel", fuel}}; }
    void loadState(const ve::Json& in) override { fuel = in.value("fuel", fuel); }

    // The nearest generator whose powerRadius reaches p (running ones only if
    // `needFuel`), or null. Placement previews (ghosts) don't count.
    static Generator* covering(const ve::Scene& scene, const glm::vec3& p, bool needFuel);
};

class PoweredLight : public ve::Component {
public:
    void update(float dt) override;
    bool lit() const { return m_lit; }

private:
    float m_sinceCheck = 1.0f; // (checked a few times a second, not every frame)
    bool m_lit = true;
};

class FuelBarrel : public ve::Component {
public:
    float litres = 20.0f;  // fuel in the barrel
    float reach = 4.0f;    // pours into a generator this close (m)

    // Pours into the nearest generator within reach and destroys the barrel;
    // returns that generator, or null (the barrel stays where it is).
    Generator* pour();
};

} // namespace savanna
