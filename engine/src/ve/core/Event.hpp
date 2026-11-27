#pragma once

// =============================================================================
// Event - a list of callbacks to notify (engine hooks: loading, shader
// compiles, ...). Main thread only.
//
//   ve::Subscription sub = engine.loadEvents().subscribe([](const ve::LoadEvent& e) { ... });
//   // unsubscribes when `sub` is destroyed (keep it as a member) or sub.reset()
//
// A callback may subscribe or unsubscribe (itself or others) while the event
// is being emitted: new subscribers are called from the next emit on, removed
// ones aren't called again. A Subscription may outlive its Event.
// =============================================================================

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace ve {

/// Keeps one callback subscribed; movable, unsubscribes when destroyed.
class Subscription {
public:
    Subscription() = default;
    /// Made by Event::subscribe; `unsubscribe` removes the callback.
    explicit Subscription(std::function<void()> unsubscribe) : m_unsubscribe(std::move(unsubscribe)) {}
    /// Takes over `o`'s subscription (`o` becomes empty).
    Subscription(Subscription&& o) noexcept : m_unsubscribe(std::exchange(o.m_unsubscribe, nullptr)) {}
    /// Unsubscribes the current callback, then takes over `o`'s.
    Subscription& operator=(Subscription&& o) noexcept
    {
        if (this != &o) {
            reset();
            m_unsubscribe = std::exchange(o.m_unsubscribe, nullptr);
        }
        return *this;
    }
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;
    ~Subscription() { reset(); }

    /// Unsubscribes now (safe to call twice, or after the Event is gone).
    void reset()
    {
        if (m_unsubscribe) std::exchange(m_unsubscribe, nullptr)();
    }
    /// Whether this still holds a subscription (reset() not called yet).
    bool active() const { return m_unsubscribe != nullptr; }

private:
    std::function<void()> m_unsubscribe;
};

template <class... Args>
/// A list of callbacks notified by emit() (an observer / signal).
/// @tparam Args what each callback receives.
class Event {
public:
    /// The callback type: `void(Args...)`.
    using Callback = std::function<void(Args...)>;

    /// Adds a callback; it's called on every emit() until the returned
    /// Subscription is destroyed or reset(). Keep the Subscription (as a member).
    [[nodiscard]] Subscription subscribe(Callback callback)
    {
        const uint64_t id = ++m_state->nextId;
        m_state->entries.push_back({id, std::make_shared<Callback>(std::move(callback))});
        std::weak_ptr<State> weak = m_state;
        return Subscription([weak, id] {
            if (auto s = weak.lock()) std::erase_if(s->entries, [id](const Entry& e) { return e.id == id; });
        });
    }

    /// Calls every subscribed callback with `args`, in subscription order.
    void emit(Args... args) const
    {
        if (m_state->entries.empty()) return;
        // A snapshot: callbacks may change the list. Ones removed meanwhile are skipped.
        const std::vector<Entry> snapshot = m_state->entries;
        for (const Entry& e : snapshot) {
            const bool still = std::any_of(m_state->entries.begin(), m_state->entries.end(), [&](const Entry& x) { return x.id == e.id; });
            if (still) (*e.callback)(args...);
        }
    }

    /// Whether nobody is subscribed.
    bool empty() const { return m_state->entries.empty(); }

private:
    struct Entry {
        uint64_t id;
        std::shared_ptr<Callback> callback;
    };
    struct State {
        std::vector<Entry> entries;
        uint64_t nextId = 0;
    };
    std::shared_ptr<State> m_state = std::make_shared<State>();
};

} // namespace ve
