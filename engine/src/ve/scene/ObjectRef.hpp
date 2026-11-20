#pragma once

// =============================================================================
// ObjectRef / ComponentRef - remembering another object or component across
// frames, safely.
//
// The editor (and gameplay) can destroy any object or remove any component at
// any time. A raw pointer kept in a member then dangles, and even calling
// isDestroyed() on it reads freed memory. These handles make the safe pattern
// ("remember the id, look it up each frame") the only one the type allows:
// there is no implicit conversion back to a pointer, you must ask a scene.
//
//   class Lamp : public ve::GameObject {
//       ve::ObjectRef<Weather> m_weather;          // stores only the 64-bit id
//       ve::ComponentRef<ve::PointLight> m_light;  // {owner id, address, serial}
//       void start() override {
//           m_weather = scene().findFirst<Weather>();
//           m_light = &addComponent<ve::PointLight>();
//       }
//       void update(float) override {
//           if (Weather* w = m_weather.get(scene()))   // null once it's gone
//               if (ve::PointLight* l = m_light.get(scene())) l->intensity = w->rain;
//       }
//   };
//
// get(scene) is an O(1) hash lookup (Scene::findById) plus, for an
// ObjectRef<T> with T != GameObject, a dynamic_cast; ComponentRef also scans
// the owner's (short) component list. Cheap enough to call every frame.
//
// get() without a scene resolves against the engine's MAIN scene
// (Engine::scene()). Objects can live in another scene, e.g. a loading
// screen's scene (Engine::loadingScene()); inside a GameObject or Component
// prefer get(scene()), which is right wherever the object lives.
//
// Ids survive save/load (GameObject::id()), so an ObjectRef can also be kept
// in data that outlives the objects. A ComponentRef cannot (component
// addresses and serials are per run).
// =============================================================================

#include "ve/scene/Scene.hpp"

#include <cstdint>
#include <type_traits>

namespace ve {

namespace detail {
Scene& mainScene(); // Engine::get().scene() (out of line: keeps Engine.hpp out of this header)
}

template <class T = GameObject>
class ObjectRef {
    // (T may be only forward-declared where an ObjectRef<T> member is
    // declared, so T is checked in get(), not here.)
public:
    ObjectRef() = default;
    // From an object (null -> empty). Implicit, so `m_ref = &obj;` works.
    ObjectRef(const T* object) : m_id(object ? object->id() : 0) {}
    // From a raw id (e.g. one read from a file). 0 = empty.
    explicit ObjectRef(uint64_t id) : m_id(id) {}
    // An ObjectRef<Derived> converts to ObjectRef<Base>.
    template <class U>
        requires(std::is_base_of_v<T, U> && !std::is_same_v<T, U>)
    ObjectRef(const ObjectRef<U>& other) : m_id(other.id())
    {
    }

    // The object if it's still alive in `scene` (and still a T), else null.
    T* get(const Scene& scene) const
    {
        static_assert(std::is_base_of_v<GameObject, T>, "ObjectRef<T>: T must be a GameObject (use ComponentRef for components)");
        GameObject* o = m_id ? scene.findById(m_id) : nullptr;
        if constexpr (std::is_same_v<T, GameObject>) return o;
        else return dynamic_cast<T*>(o);
    }
    // The same in the engine's main scene (see the header comment).
    T* get() const { return get(detail::mainScene()); }

    uint64_t id() const { return m_id; }
    void reset() { m_id = 0; }
    // Nothing remembered. (Not the same as "alive": a non-empty ref can still
    // resolve to null once the object is destroyed.)
    bool empty() const { return m_id == 0; }

    bool operator==(const ObjectRef&) const = default;

private:
    uint64_t m_id = 0;
};

// A remembered component. An address alone isn't enough: once a component is
// freed, the allocator may put a new one (even of the same type) at the same
// address. So the ref also keeps the component's serial (unique per run,
// never reused) and only trusts the pointer after finding that exact address
// in the owner's live components() AND checking the serial there.
template <class T>
class ComponentRef {
public:
    ComponentRef() = default;
    // From an attached component (null or not yet attached -> empty).
    ComponentRef(const T* component)
    {
        if (!component || component->serial() == 0) return; // serial 0: not attached to an object yet
        m_owner = &component->gameObject();
        m_component = component;
        m_serial = component->serial();
    }

    // The component if it's still attached to its (live) owner in `scene`.
    T* get(const Scene& scene) const
    {
        static_assert(std::is_base_of_v<Component, T>, "ComponentRef<T>: T must be a Component");
        GameObject* owner = m_owner.get(scene);
        if (!owner) return nullptr;
        // Compare addresses only: m_component may point at freed memory.
        for (const auto& c : owner->components()) {
            if (c.get() != m_component) continue;
            // Same address, and now we know it's live: is it the same component?
            if (c->serial() != m_serial) return nullptr;
            return static_cast<T*>(c.get()); // it is the T we were made from
        }
        return nullptr;
    }
    T* get() const { return get(detail::mainScene()); }

    // The owning object's ref (its id), e.g. to find it again.
    const ObjectRef<>& owner() const { return m_owner; }
    void reset() { *this = {}; }
    bool empty() const { return m_serial == 0; }

    bool operator==(const ComponentRef&) const = default;

private:
    ObjectRef<> m_owner;
    const Component* m_component = nullptr; // never dereferenced before verification
    uint64_t m_serial = 0;
};

} // namespace ve
