#pragma once

// =============================================================================
// ItemStacks (game side) - stacks of items in a fixed number of slots
// (backpacks, chests, hotbars): the bookkeeping behind Savanna's Inventory.
// What the items are, how many stack and the screen are in Inventory.*.
// (Once ve::Inventory: inventories are game rules, so they live in the game.)
//
//   savanna::ItemStacks bag(24, [](const std::string& item) { return item == "arrow" ? 64 : 10; });
//   const int left = bag.add("arrow", 100);  // 0: two stacks (64 + 36)
//   bag.move(0, 5);                          // merge into slot 5, or swap
//   game["bag"] = bag.save();                // [{"item": "arrow", "count": 64}, null, ...]
//
// Items are plain ids ("wood", "berries"); an empty id is a free slot.
// =============================================================================

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

namespace savanna {

using Json = nlohmann::json;

/// A grid of item stacks (items by name), e.g. a backpack or a chest. Plain
/// data: no UI, no GameObject; save it with toJson / fromJson.
class ItemStacks {
public:
    /// Items the max-stack function doesn't know about (or with none set).
    static constexpr int kDefaultMaxStack = 99;

    /// One slot's contents.
    struct Stack {
        std::string item; ///< empty = free slot
        int count = 0;  ///< How many (0 for a free slot).
    };
    /// How many of an item fit in one slot.
    using MaxStack = std::function<int(const std::string& item)>;

    /// An inventory with `slotCount` empty slots; `maxStack` says how many of an item fit in a slot (none: kDefaultMaxStack).
    explicit ItemStacks(int slotCount, MaxStack maxStack = {});

    std::vector<Stack> slots; ///< always slotCount long

    /// Number of slots.
    int size() const { return int(slots.size()); }
    int maxStack(const std::string& item) const; ///< the function's answer, or kDefaultMaxStack
    int add(const std::string& item, int count);    ///< tops up stacks, then fills free slots; returns how many didn't fit
    int remove(const std::string& item, int count); ///< from the last slots first; returns how many were taken
    /// How many of `item` it holds in all.
    int count(const std::string& item) const;
    void move(int from, int to);                    ///< merge what fits, or swap
    /// Empties every slot.
    void clear();

    /// One entry per slot: {"item", "count"}, or null for a free slot.
    Json save() const;
    void load(const Json& j); ///< (extra entries are ignored, missing ones left free)

private:
    MaxStack m_maxStack;
};

} // namespace savanna
