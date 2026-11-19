#include "Harvest.hpp"

#include "Player.hpp"
#include "Props.hpp"

#include <ve/gameplay/Interaction.hpp>
#include <ve/physics/Physics.hpp>
#include <ve/scene/Reflection.hpp>
#include <ve/scene/Scene.hpp>
#include <ve/world/Foliage.hpp>
#include <ve/world/Vegetation.hpp>

namespace savanna {

VE_REGISTER_TYPE(ChoppableTree, "ChoppableTree") { t.category("Savanna"); }
VE_REGISTER_TYPE(BerryBush, "BerryBush") { t.category("Savanna"); }

// -----------------------------------------------------------------------------
// Harvestable
// -----------------------------------------------------------------------------

ve::FoliageLink* Harvestable::link() const
{
    return getComponent<ve::FoliageLink>();
}

void Harvestable::start()
{
    // The engine finds it (raycast from the player's eye) and fires Use.
    auto& i = addComponent<ve::Interactable>();
    i.promptFn = [this] { return prompt(); };
    i.onUse = [this](ve::GameObject&) { use(); };
}

// -----------------------------------------------------------------------------
// ChoppableTree
// -----------------------------------------------------------------------------

std::string ChoppableTree::prompt() const
{
    const ve::FoliageLink* l = link();
    if (!l || l->info.state != 0) return {}; // already a stump
    return "Chop tree (" + std::to_string(chops) + "/" + std::to_string(kChops) + ")";
}

bool ChoppableTree::chop()
{
    ve::FoliageLink* l = link();
    if (!l || l->info.state != 0) return false;
    if (++chops < kChops) return false;
    GameState& gs = gameState();
    gs.inventory.add("wood", 4);
    gs.message = "Timber! +4 wood";
    gs.messageTimer = 2.0f;
    // A stump from now on: the foliage system keeps it so, and swaps this
    // object for a fresh one showing the stump.
    l->setState("Stump");
    return true;
}

// -----------------------------------------------------------------------------
// BerryBush
// -----------------------------------------------------------------------------

std::string BerryBush::prompt() const
{
    const ve::FoliageLink* l = link();
    return l && l->info.state == 0 ? "Pick berries" : std::string();
}

bool BerryBush::pick()
{
    ve::FoliageLink* l = link();
    if (!l || l->info.state != 0) return false;
    GameState& gs = gameState();
    gs.inventory.add("berries", 6);
    gs.message = "+6 berries";
    gs.messageTimer = 2.0f;
    if (Regrowth* r = Regrowth::find(scene())) r->schedule(l->info.id.packed(), r->regrowSeconds);
    l->setState("Picked");
    return true;
}

// -----------------------------------------------------------------------------
// Regrowth
// -----------------------------------------------------------------------------

Regrowth* Regrowth::find(ve::Scene& scene)
{
    return scene.findFirst<Regrowth>();
}

void Regrowth::saveState(ve::Json& out) const
{
    ve::Json due = ve::Json::array();
    for (const auto& [id, t] : m_due) due.push_back({id, t - m_time}); // seconds left
    out["due"] = std::move(due);
}

void Regrowth::loadState(const ve::Json& in)
{
    m_due.clear();
    for (const ve::Json& e : in.value("due", ve::Json::array()))
        if (e.is_array() && e.size() == 2) m_due[e[0].get<uint64_t>()] = m_time + e[1].get<float>();
}

void Regrowth::schedule(uint64_t instanceId, float seconds)
{
    m_due[instanceId] = m_time + seconds;
}

void Regrowth::update(float dt)
{
    m_time += dt;
    auto* foliageObject = scene().findFirst<ve::Foliage>();
    ve::FoliageSystem* foliage = foliageObject ? foliageObject->system() : nullptr;
    if (!foliage) return;
    for (auto it = m_due.begin(); it != m_due.end();) {
        if (m_time < it->second) {
            ++it;
            continue;
        }
        foliage->setState(ve::FoliageInstanceId::unpack(it->first), 0); // berries are back
        it = m_due.erase(it);
    }
}

} // namespace savanna
