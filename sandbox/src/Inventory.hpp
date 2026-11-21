#pragma once

// The player's inventory (game side): the items there are, and the
// inventory screen (Tab / I): a grid of slots built from the engine's UI elements,
// with drag & drop (or click a slot, then another) to move / merge stacks,
// tooltips and a details panel. Item icons are baked from small meshes
// (ve::ui::bakeIcon) when the screen is first built.

#include <ve/gameplay/Inventory.hpp>
#include <ve/render/Mesh.hpp>
#include <ve/scene/Reflection.hpp>
#include <ve/ui/Gui.hpp>

#include <memory>
#include <string>

namespace ve {
class Texture;
}

namespace savanna {

struct ItemType {
    std::string id, name, description;
    int maxStack = 50;
    std::shared_ptr<ve::Texture> icon; // baked on first use
};
const ItemType* itemType(const std::string& id);
void bakeItemIcons(); // all of them, once (needs the renderer)
// The item's small mesh (vertex-coloured, about a metre across): icons are
// baked from it, and pickups lying in the world use it scaled down (Pickups.hpp).
ve::MeshData itemMesh(const std::string& id);

// The player's inventory: the engine's container (ve/gameplay/Inventory.hpp)
// with Savanna's slot count and the item types' stack limits.
class Inventory : public ve::Inventory {
public:
    static constexpr int kSlots = 24;
    Inventory();
};

// The screen. Lives on a GameObject with a ve::ui::Canvas.
class InventoryScreen : public ve::GameObject {
public:
    InventoryScreen();
    void start() override;
    void update(float dt) override;
    bool open() const;
    void setOpen(bool open);
    void refresh(); // rebuild the slots from the inventory
    ve::ui::Canvas& canvas() { return *m_canvas; }
    int selectedSlot() const { return m_selected; }
    void clickSlot(int index); // as the player would (tests)

private:
    void build();
    void showDetails(int index);
    ve::ui::Canvas* m_canvas = nullptr;
    ve::ui::Grid* m_grid = nullptr;
    ve::ui::Label* m_detailName = nullptr;
    ve::ui::Label* m_detailText = nullptr;
    ve::ui::Image* m_detailIcon = nullptr;
    int m_selected = -1; // picked up with a click / pad A
    int m_shown = -1;    // details panel
};

} // namespace savanna
