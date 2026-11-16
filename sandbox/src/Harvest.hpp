#pragma once

// =============================================================================
// Harvest - interactive foliage, Savanna's gameplay side.
//
// The engine streams the trees and bushes as instances and, near the player,
// promotes them to GameObjects of these classes (FoliageType::promoteType).
// What chopping and picking mean lives here: a tree takes three chops and
// becomes a stump for the rest of the game, a berry bush is picked and its
// berries grow back later. The engine only remembers each instance's state
// (FoliageSystem::setState) so it stays that way however far you walk.
// =============================================================================

#include <ve/scene/GameObject.hpp>

#include <cstdint>
#include <unordered_map>

namespace ve {
class FoliageLink;
}

namespace savanna {

class Player;

// Interaction: the player's "Use" key while looking at this object up close.
class Interactable : public ve::GameObject {
public:
    explicit Interactable(std::string name) : GameObject(std::move(name)) {}
    void update(float dt) override;

protected:
    // Prompt shown while looked at ("" = nothing to do), and the action.
    virtual std::string prompt() const = 0;
    virtual void use() = 0;
    ve::FoliageLink* link() const;
};

class ChoppableTree : public Interactable {
public:
    static constexpr int kChops = 3;
    ChoppableTree() : Interactable("Choppable tree") {}
    // One swing; on the last it falls and leaves a stump for good. Returns true when felled.
    bool chop();
    int chops = 0;

protected:
    std::string prompt() const override;
    void use() override { chop(); }
};

class BerryBush : public Interactable {
public:
    BerryBush() : Interactable("Berry bush") {}
    bool pick(); // false if already picked

protected:
    std::string prompt() const override;
    void use() override { pick(); }
};

// Puts picked bushes' berries back after regrowSeconds of game time (the
// bookkeeping is the game's: the engine only knows each bush's state).
class Regrowth : public ve::GameObject {
public:
    Regrowth() : GameObject("Regrowth") {}
    void update(float dt) override;
    void schedule(uint64_t instanceId, float seconds);
    float regrowSeconds = 120.0f;
    static Regrowth* find(ve::Scene& scene);

private:
    std::unordered_map<uint64_t, float> m_due; // instance id -> game time
    float m_time = 0.0f;
};

} // namespace savanna
