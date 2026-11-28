#include "Inventory.hpp"

#include "GameUi.hpp"
#include "Building.hpp"
#include "Props.hpp"

#include <ve/core/Engine.hpp>
#include <ve/core/Input.hpp>
#include <ve/render/Primitives.hpp>
#include <ve/render/Renderer.hpp>
#include <ve/scene/Components.hpp>
#include <ve/scene/Scene.hpp>
#include <ve/ui/IconBaker.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <map>

namespace savanna {

using namespace ve::ui;

// -----------------------------------------------------------------------------
// Items
// -----------------------------------------------------------------------------

namespace {

std::map<std::string, ItemType>& items()
{
    static std::map<std::string, ItemType> m = {
        {"wood", {"wood", "Wood", "Split acacia logs. Burns well; the start of every hut and fence.", 50}},
        {"berries", {"berries", "Berries", "Sweet and a little sour. Grow back on the bush after a while.", 30}},
        {"sunstone", {"sunstone", "Sunstone", "Warm to the touch, it glows faintly at dusk. Twelve are hidden around the savanna.", 12}},
        {"stone", {"stone", "Stone", "A fist-sized stone from the plain. Good for a fire ring, or for throwing.", 40}},
        {"stick", {"stick", "Stick", "A dry branch, fallen from an acacia. Kindling, or the start of a tool.", 40}},
    };
    return m;
}

} // namespace

ve::MeshData itemMesh(const std::string& id)
{
    namespace prim = ve::primitives;
    ve::MeshData m;
    auto at = [](glm::vec3 p, float yaw = 0.0f, float roll = 0.0f) {
        glm::mat4 t = glm::translate(glm::mat4(1.0f), p);
        t = glm::rotate(t, yaw, {0, 1, 0});
        return glm::rotate(t, roll, {0, 0, 1});
    };
    if (id == "wood") { // three logs, cut ends showing
        const glm::vec4 bark(0.36f, 0.24f, 0.15f, 1.0f), cut(0.80f, 0.64f, 0.42f, 1.0f);
        const glm::vec3 pos[3] = {{-0.23f, 0.2f, 0.0f}, {0.23f, 0.2f, 0.0f}, {0.0f, 0.58f, 0.0f}};
        for (const glm::vec3& p : pos) {
            const glm::mat4 t = at(p, glm::radians(-25.0f), glm::radians(90.0f));
            m.append(prim::cylinder(0.22f, 0.22f, 1.6f, 9, false), t, bark);
            m.append(prim::cylinder(0.2f, 0.2f, 0.02f, 9, true), t * glm::translate(glm::mat4(1.0f), {0.0f, 0.8f, 0.0f}), cut);
            m.append(prim::cylinder(0.2f, 0.2f, 0.02f, 9, true), t * glm::translate(glm::mat4(1.0f), {0.0f, -0.81f, 0.0f}), cut);
        }
    } else if (id == "berries") { // a cluster on a stem with two leaves
        const glm::vec3 pos[7] = {{0, 0, 0}, {0.3f, 0.05f, 0.1f}, {-0.28f, 0.03f, 0.12f}, {0.08f, 0.04f, -0.3f}, {0.15f, 0.3f, -0.05f}, {-0.15f, 0.28f, 0.05f}, {0.0f, 0.55f, 0.0f}};
        for (int i = 0; i < 7; ++i)
            m.append(prim::icosphere(0.2f, 2), at(pos[i]), glm::vec4(0.72f + 0.04f * float(i % 3), 0.08f, 0.12f, 1.0f));
        m.append(prim::cylinder(0.025f, 0.02f, 0.4f, 6), at({0.0f, 0.75f, 0.0f}), glm::vec4(0.3f, 0.35f, 0.15f, 1.0f));
        m.append(prim::cube({0.45f, 0.02f, 0.2f}), at({0.2f, 0.85f, 0.0f}, 0.0f, glm::radians(25.0f)), glm::vec4(0.3f, 0.5f, 0.2f, 1.0f));
        m.append(prim::cube({0.45f, 0.02f, 0.2f}), at({-0.2f, 0.85f, 0.0f}, 0.0f, glm::radians(-25.0f)), glm::vec4(0.28f, 0.46f, 0.18f, 1.0f));
    } else if (id == "stone") { // a flattish, faceted pebble resting on its base
        m.append(prim::icosphere(0.5f, 1), at({0.0f, 0.28f, 0.0f}) * glm::scale(glm::mat4(1.0f), {1.15f, 0.6f, 0.9f}),
                 glm::vec4(0.47f, 0.44f, 0.40f, 1.0f));
    } else if (id == "stick") { // a dry branch lying on the ground, with one twig
        const glm::vec4 bark(0.42f, 0.30f, 0.19f, 1.0f);
        m.append(prim::cylinder(0.09f, 0.06f, 1.6f, 6), at({0.0f, 0.09f, 0.0f}, 0.0f, glm::radians(90.0f)), bark);
        m.append(prim::cylinder(0.05f, 0.025f, 0.5f, 5), at({0.25f, 0.09f, 0.12f}, glm::radians(-40.0f), glm::radians(90.0f)) *
                     glm::translate(glm::mat4(1.0f), {0.0f, 0.25f, 0.0f}), bark);
    } else { // sunstone: a faceted golden gem
        m.append(prim::icosphere(0.55f, 0), glm::scale(glm::mat4(1.0f), {1.0f, 1.25f, 1.0f}), glm::vec4(1.0f, 0.72f, 0.25f, 1.0f));
    }
    m.makeFlatShaded();
    return m;
}

const ItemType* itemType(const std::string& id)
{
    auto it = items().find(id);
    return it != items().end() ? &it->second : nullptr;
}

void bakeItemIcons()
{
    static bool releaseHooked = false;
    if (!releaseHooked) { // the icons live in a static table: drop them before the renderer goes
        releaseHooked = true;
        ve::Engine::get().atShutdown([] {
            for (auto& [id, type] : items()) type.icon.reset();
        });
    }
    for (auto& [id, type] : items()) {
        if (type.icon) continue;
        ve::ui::IconView view;
        view.pitchDegrees = 30.0f;
        type.icon = ve::ui::bakeIcon(ve::Engine::get().renderer(), itemMesh(id), 128, view);
    }
}

// -----------------------------------------------------------------------------
// Inventory
// -----------------------------------------------------------------------------

Inventory::Inventory()
    : ItemStacks(kSlots, [](const std::string& item) {
          const ItemType* t = itemType(item);
          return t ? t->maxStack : kDefaultMaxStack;
      })
{
}

// -----------------------------------------------------------------------------
// The screen
// -----------------------------------------------------------------------------

namespace {
constexpr int kColumns = 8;
constexpr float kCell = 92.0f;
} // namespace

InventoryScreen::InventoryScreen() : GameObject("Inventory screen")
{
    m_canvas = &addComponent<Canvas>();
    m_canvas->modal = true;
    m_canvas->visible = false;
    m_canvas->order = 20;
    m_canvas->tooltipFont = gui::font::body;
    m_canvas->onBack = [this] { setOpen(false); };
    m_canvas->shortcuts = {{ve::Key::Tab, ve::GamepadButton::Y, [this] { setOpen(false); }}, {ve::Key::I, ve::GamepadButton::Count, [this] { setOpen(false); }}};
    input().bindAction("Inventory", ve::Key::Tab);
    input().bindAction("Inventory", ve::Key::I);
    input().bindAction("Inventory", ve::GamepadButton::Y);
}

void InventoryScreen::start()
{
    build();
}

bool InventoryScreen::open() const
{
    return m_canvas->visible;
}

void InventoryScreen::setOpen(bool on)
{
    if (on == open()) return;
    m_canvas->visible = on;
    m_selected = -1;
    if (on) {
        refresh();
        m_canvas->focus(nullptr);
    }
    input().setMouseCaptured(!on); // a cursor while it's open
}

void InventoryScreen::update(float)
{
    // Tab / I / pad Y open it (gameplay input: blocked while another menu is up) and close it.
    // (While it's open the canvas is modal: Tab / I / pad Y and Escape / pad B close it through its shortcuts / onBack.)
    if (!open() && input().actionPressed("Inventory") && !Builder::building(scene())) setOpen(true);
}

void InventoryScreen::build()
{
    bakeItemIcons();
    Element& root = m_canvas->root();
    // Dim the world behind.
    auto& dim = root.add<Panel>("dim");
    dim.fill();
    dim.color = ve::ui::rgba(0x00000070);
    dim.blocksPointer = true;

    Grid layout;
    layout.columns = kColumns;
    layout.cell = {kCell, kCell};
    layout.spacing = {8.0f, 8.0f};
    const glm::vec2 slotsSize = layout.contentSize(Inventory::kSlots);

    Element& content = gui::window(root, "Inventory", {slotsSize.x + 48.0f + 340.0f, slotsSize.y + 104.0f + 56.0f});
    auto& grid = content.add<Grid>("slots");
    grid.columns = kColumns;
    grid.cell = layout.cell;
    grid.spacing = layout.spacing;
    grid.at({0.0f, 0.0f}).size(slotsSize);
    m_grid = &grid;

    // Details on the right.
    auto& details = content.add<Panel>("details");
    details.anchors({1.0f, 0.0f}, {1.0f, 1.0f}).pivot({1.0f, 0.0f}).at({0.0f, 0.0f}).size({316.0f, 56.0f});
    details.color = gui::color::slot;
    details.rounding = 4.0f;
    auto& icon = details.add<Image>("icon");
    icon.anchor({0.5f, 0.0f}).pivot({0.5f, 0.0f}).at({0.0f, 14.0f}).size({128.0f, 128.0f});
    m_detailIcon = &icon;
    m_detailName = &gui::label(details, "", gui::font::heading, 30.0f, gui::color::gold);
    m_detailName->anchors({0.0f, 0.0f}, {1.0f, 0.0f}).at({16.0f, 150.0f}).size({16.0f, 40.0f});
    m_detailName->alignX = Align::Center;
    m_detailText = &gui::label(details, "", gui::font::body, 20.0f, gui::color::text);
    m_detailText->anchors({0.0f, 0.0f}, {1.0f, 1.0f}).at({20.0f, 196.0f}).size({20.0f, 16.0f});
    m_detailText->wrap = true;
    m_detailText->alignY = Align::Start;

    // Hints along the bottom.
    auto& hint = gui::label(content, "Drag to move or stack   ·   click a slot, then another   ·   Tab / Esc close", gui::font::body, 18.0f,
                            gui::color::dim);
    hint.anchors({0.0f, 1.0f}, {1.0f, 1.0f}).pivot({0.0f, 1.0f}).at({0.0f, 0.0f}).size({0.0f, 28.0f});
    refresh();
}

void InventoryScreen::refresh()
{
    if (!m_grid) return;
    m_grid->clear();
    Inventory& inv = gameState().inventory;
    for (int i = 0; i < Inventory::kSlots; ++i) {
        const Inventory::Stack s = inv.slots[size_t(i)];
        const ItemType* type = s.item.empty() ? nullptr : itemType(s.item);
        auto& slot = m_grid->add<Button>("", "slot" + std::to_string(i));
        slot.normal = gui::color::slot;
        slot.hover = ve::ui::rgba(0x3a2a18e8);
        slot.down = ve::ui::rgba(0x5a3e20ff);
        slot.borderColor = i == m_selected ? gui::color::gold : ve::ui::withAlpha(gui::color::goldDark, 0.55f);
        slot.borderWidth = i == m_selected ? 2.5f : 1.0f;
        slot.rounding = 4.0f;
        slot.focusBorder = gui::color::gold;
        slot.selected = i == m_selected;
        if (type) {
            slot.tooltip = type->name;
            auto& img = slot.add<Image>("icon");
            img.fill(10.0f);
            img.texture = type->icon;
            auto& n = gui::label(slot, std::to_string(s.count), gui::font::bold, 20.0f, gui::color::text);
            n.fill({0.0f, 0.0f, 8.0f, 4.0f});
            n.alignX = Align::End;
            n.alignY = Align::End;
            slot.draggable = true;
            slot.dragVisual = [icon = type->icon](Painter& p, glm::vec2 at) {
                p.image(icon, {at - glm::vec2(36.0f), at + glm::vec2(36.0f)}, glm::vec4(1.0f, 1.0f, 1.0f, 0.85f));
            };
        }
        slot.onHover = [this, i](bool on) {
            if (on) showDetails(i);
        };
        slot.onClick = [this, i] { clickSlot(i); };
        slot.onDrop = [this, i](Element& dragged) {
            // Dragged from another slot: its index is in its name.
            const std::string& n = dragged.name;
            if (n.rfind("slot", 0) != 0) return false;
            gameState().inventory.move(std::stoi(n.substr(4)), i);
            m_selected = -1;
            refresh();
            showDetails(i);
            return true;
        };
    }
    showDetails(m_shown >= 0 ? m_shown : 0);
}

void InventoryScreen::clickSlot(int i)
{
    Inventory& inv = gameState().inventory;
    if (m_selected < 0) {
        if (!inv.slots[size_t(i)].item.empty()) m_selected = i; // picked up
    } else {
        inv.move(m_selected, i); // put down: move, stack or swap
        m_selected = -1;
    }
    // Rebuilding the slots would drop the focus: keep it on the same slot.
    refresh();
    if (Element* e = m_grid->find("slot" + std::to_string(i))) m_canvas->focus(e);
    showDetails(i);
}

void InventoryScreen::showDetails(int i)
{
    m_shown = i;
    const Inventory::Stack& s = gameState().inventory.slots[size_t(std::clamp(i, 0, Inventory::kSlots - 1))];
    const ItemType* type = s.item.empty() ? nullptr : itemType(s.item);
    m_detailIcon->texture = type ? type->icon : nullptr;
    m_detailName->text = type ? type->name : "Empty";
    m_detailText->text = type ? type->description + "\n\nIn this stack: " + std::to_string(s.count) + " / " + std::to_string(type->maxStack)
                              : "Gather things in the savanna: chop trees, pick berries, find sunstones.";
}

} // namespace savanna
