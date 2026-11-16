#include "Harvest.hpp"

#include "Player.hpp"
#include "Props.hpp"

#include <ve/physics/Physics.hpp>
#include <ve/scene/Reflection.hpp>
#include <ve/scene/Scene.hpp>
#include <ve/world/Foliage.hpp>
#include <ve/world/Vegetation.hpp>

namespace savanna {

VE_REGISTER_TYPE(ChoppableTree, "ChoppableTree") { t.category("Savanna"); }
VE_REGISTER_TYPE(BerryBush, "BerryBush") { t.category("Savanna"); }

// -----------------------------------------------------------------------------
// Interactable
// -----------------------------------------------------------------------------

ve::FoliageLink* Interactable::link() const
{
    return getComponent<ve::FoliageLink>();
}

void Interactable::update(float)
{
    auto* player = dynamic_cast<Player*>(scene().find("Player"));
    if (!player) return;
    // Looked at within reach: the ray from the eye hits this object's collider.
    const auto hit = physics().raycast(player->eyePosition(), player->lookDirection(), 3.0f);
    if (!hit || hit->gameObject != this) return;
    const std::string text = prompt();
    if (text.empty()) return;
    GameState& gs = gameState();
    gs.message = text;
    gs.messageTimer = 0.1f;
    if (player->usePressed()) use();
}

// -----------------------------------------------------------------------------
// ChoppableTree
// -----------------------------------------------------------------------------

std::string ChoppableTree::prompt() const
{
    const ve::FoliageLink* l = link();
    if (!l || l->info.state != 0) return {}; // already a stump
    return "[E] Chop tree (" + std::to_string(chops) + "/" + std::to_string(kChops) + ")";
}

bool ChoppableTree::chop()
{
    ve::FoliageLink* l = link();
    if (!l || l->info.state != 0) return false;
    if (++chops < kChops) return false;
    GameState& gs = gameState();
    gs.wood += 4;
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
    return l && l->info.state == 0 ? "[E] Pick berries" : std::string();
}

bool BerryBush::pick()
{
    ve::FoliageLink* l = link();
    if (!l || l->info.state != 0) return false;
    GameState& gs = gameState();
    gs.berries += 6;
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
