#pragma once

// =============================================================================
// Pickups - stones and sticks lying all over the savanna, to pick up.
//
// Thousands of them, so they aren't GameObjects: one ve::ProxySet holds them
// as data and draws them instanced. Within a few metres of the player the
// set turns each into a Pickup (a GameObject with a ve::Interactable: "Pick
// up stone"); using it puts the item in the inventory and destroys the
// object, which removes the entry for good. Save games store only the
// difference to the level's scatter (the set is Saveable), so picking up a
// few stones saves a few ids.
//
// The scatter is a pure function of the terrain (fixed seed, own hash: the
// same on every platform), so the entries get the same ids every run.
// =============================================================================

#include <ve/scene/GameObject.hpp>

#include <string>

namespace ve {
class ProxySet;
class Scene;
class Terrain;
} // namespace ve

namespace savanna {

class Player;

// Spawns the "Pickups" set and scatters stones and sticks over the terrain;
// promotion follows `player`.
ve::ProxySet& spawnPickups(ve::Scene& scene, const ve::Terrain& terrain, const Player& player);
ve::ProxySet* findPickups(const ve::Scene& scene);

// A promoted stone or stick (the set places it and gives it the instance's mesh).
class Pickup : public ve::GameObject {
public:
    explicit Pickup(std::string item = "stone");
    void start() override;
    void pickUp(); // into the inventory; the object (and its entry) goes
    const std::string& item() const { return m_item; }

private:
    std::string m_item;
};

} // namespace savanna
