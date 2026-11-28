#include "ItemStacks.hpp"

#include <algorithm>
#include <utility>

namespace savanna {

ItemStacks::ItemStacks(int slotCount, MaxStack maxStack) : slots(size_t(std::max(slotCount, 0))), m_maxStack(std::move(maxStack)) {}

int ItemStacks::maxStack(const std::string& item) const
{
    return m_maxStack ? m_maxStack(item) : kDefaultMaxStack;
}

int ItemStacks::add(const std::string& item, int count)
{
    const int limit = maxStack(item);
    for (Stack& s : slots) // top up existing stacks first
        if (count > 0 && s.item == item && s.count < limit) {
            const int n = std::min(count, limit - s.count);
            s.count += n;
            count -= n;
        }
    for (Stack& s : slots)
        if (count > 0 && s.item.empty()) {
            const int n = std::min(count, limit);
            s = {item, n};
            count -= n;
        }
    return count;
}

int ItemStacks::remove(const std::string& item, int count)
{
    int taken = 0;
    for (auto it = slots.rbegin(); it != slots.rend() && taken < count; ++it)
        if (it->item == item) {
            const int n = std::min(count - taken, it->count);
            it->count -= n;
            taken += n;
            if (it->count == 0) *it = {};
        }
    return taken;
}

int ItemStacks::count(const std::string& item) const
{
    int n = 0;
    for (const Stack& s : slots)
        if (s.item == item) n += s.count;
    return n;
}

void ItemStacks::move(int from, int to)
{
    if (from == to || from < 0 || to < 0 || from >= size() || to >= size() || slots[size_t(from)].item.empty()) return;
    Stack& a = slots[size_t(from)];
    Stack& b = slots[size_t(to)];
    if (b.item == a.item) { // merge what fits
        const int n = std::min(a.count, maxStack(a.item) - b.count);
        b.count += n;
        a.count -= n;
        if (a.count == 0) a = {};
    } else {
        std::swap(a, b);
    }
}

void ItemStacks::clear()
{
    std::fill(slots.begin(), slots.end(), Stack{});
}

Json ItemStacks::save() const
{
    Json j = Json::array();
    for (const Stack& s : slots) j.push_back(s.item.empty() ? Json(nullptr) : Json{{"item", s.item}, {"count", s.count}});
    return j;
}

void ItemStacks::load(const Json& j)
{
    clear();
    if (!j.is_array()) return;
    for (size_t i = 0; i < j.size() && i < slots.size(); ++i)
        if (j[i].is_object()) slots[i] = {j[i].value("item", std::string()), j[i].value("count", 0)};
}

} // namespace savanna
