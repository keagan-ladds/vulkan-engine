#pragma once

// =============================================================================
// GameObject & Component - Unity-style gameplay objects.
//
// Write behaviour by deriving from GameObject (or Component) and overriding
// the lifecycle hooks:
//
//   class Spinner : public ve::GameObject {
//   public:
//       Spinner() : GameObject("Spinner") {
//           addComponent<ve::MeshRenderer>(mesh, material);   // what it looks like
//       }
//       void update(float dt) override {
//           transform().rotate({0, 1, 0}, dt);                 // what it does
//           if (input().keyPressed(ve::Key::Space)) destroy();
//       }
//   };
//   scene.spawn<Spinner>().transform().setPosition({0, 1, 0});
//
// Hook order each frame:  start() (once, before the first update)
//   -> fixedUpdate(fixedDt) x N -> update(dt) -> lateUpdate(dt) -> onGui()
// destroy() is deferred to the end of the frame; onDestroy() is then called.
//
// Hooks you don't override cost nothing: the base versions of update,
// fixedUpdate, lateUpdate and onGui mark themselves as "not overridden" the
// first time they run, and the Scene stops calling them. (So don't call the
// base class's empty hook, e.g. ve::GameObject::update(dt), from an override.)
// =============================================================================

#include "ve/scene/Transform.hpp"

#include <memory>
#include <string>
#include <typeindex>
#include <vector>

namespace ve {

struct Collision; // ve/physics/Physics.hpp

// Per-frame hooks the Scene dispatches; used as bits in "not overridden" masks.
enum HookBit : uint8_t { kHookUpdate = 1, kHookFixedUpdate = 2, kHookLateUpdate = 4, kHookGui = 8 };

class Scene;
class GameObject;
class Engine;
class Input;
class Renderer;
class AssetManager;
class AudioSystem;
class PhysicsWorld;
namespace net {
class Network;
}
struct Time;

// Shortcuts to engine systems, available in both GameObject and Component.
class EngineAccess {
public:
    static Engine& engine();
    static Input& input();
    static const Time& time();
    static Renderer& renderer();
    static AssetManager& assets();
    static AudioSystem& audio();
    static PhysicsWorld& physics();
    static net::Network& net();
};

// The lifecycle and physics hooks GameObject and Component share. Override
// the ones you need; see the hook order at the top of this file.
class Behaviour : public EngineAccess {
public:
    virtual ~Behaviour() = default;

    virtual void start() {}
    virtual void update(float /*dt*/) { m_noHook |= kHookUpdate; }
    virtual void fixedUpdate(float /*dt*/) { m_noHook |= kHookFixedUpdate; }
    virtual void lateUpdate(float /*dt*/) { m_noHook |= kHookLateUpdate; }
    virtual void onGui() { m_noHook |= kHookGui; }
    virtual void onDestroy() {}
    // Physics events (ve/physics/Physics.hpp), sent during the fixed update to
    // the GameObjects on both sides of a contact and all their components.
    virtual void onCollisionEnter(const Collision&) {}
    virtual void onCollisionStay(const Collision&) {}
    virtual void onCollisionExit(const Collision&) {}
    virtual void onTriggerEnter(GameObject& /*other*/) {}
    virtual void onTriggerExit(GameObject& /*other*/) {}

private:
    friend class Scene;
    uint8_t m_noHook = 0; // HookBits whose base (empty) version ran: not overridden
};

class Component : public Behaviour {
public:

    GameObject& gameObject() const { return *m_owner; }
    Transform& transform() const;
    // Enabled and the owner is active in the hierarchy.
    bool isActive() const;

    bool enabled = true;
    // removeComponent() was called: no more hooks, and the owner frees it at
    // the end of the frame (until then it is still in components()).
    bool isRemoved() const { return m_removed; }
    // Saved in scene files (set by the editor / scene loader). Components a
    // GameObject class creates for itself stay false.
    bool persistent = false;

private:
    friend class GameObject;
    friend class Scene;
    GameObject* m_owner = nullptr;
    bool m_started = false;
    bool m_removed = false;
    uint32_t m_bucketIndex = 0; // position in the Scene's registry bucket (O(1) unregister)
    std::type_index m_type = typeid(Component);
};

class GameObject : public Behaviour {
public:
    explicit GameObject(std::string name = "GameObject");
    virtual ~GameObject();

    GameObject(const GameObject&) = delete;
    GameObject& operator=(const GameObject&) = delete;

    // Lifecycle and physics hooks: see Behaviour.

    // --- Identity & state -------------------------------------------------------
    const std::string& name() const { return m_name; }
    void setName(std::string name) { m_name = std::move(name); }
    Transform& transform() { return m_transform; }
    const Transform& transform() const { return m_transform; }
    bool activeSelf() const { return m_active; }
    bool activeInHierarchy() const;
    void setActive(bool active) { m_active = active; }
    void destroy(); // deferred to end of frame, also destroys children
    bool isDestroyed() const { return m_destroyed; }
    Scene& scene() const { return *m_scene; }
    bool inScene(const Scene& scene) const { return m_scene == &scene; } // false until spawned
    std::string tag; // free-form label for gameplay queries

    // --- Editor / scene files ---------------------------------------------------
    uint64_t id() const { return m_id; } // random, stable across save/load
    void setId(uint64_t id);
    bool persistent = false;      // saved in scene files (editor-created / loaded objects)
    bool runInEditor = false;     // keeps updating while the editor has the game paused
    bool hideInHierarchy = false; // editor internals (editor camera)

    // Convenience: parent this object's transform under another object.
    void setParent(GameObject* parent, bool keepWorld = true);

    // --- Components -------------------------------------------------------------
    template <class T, class... Args>
    T& addComponent(Args&&... args)
    {
        auto c = std::make_unique<T>(std::forward<Args>(args)...);
        T& ref = *c;
        attachComponent(std::move(c), typeid(T));
        return ref;
    }

    template <class T>
    T* getComponent() const
    {
        for (const auto& c : m_components) {
            if (c->m_removed) continue;
            if (auto* t = dynamic_cast<T*>(c.get())) return t;
        }
        return nullptr;
    }

    template <class T>
    T* getComponentInChildren() const
    {
        if (T* t = getComponent<T>()) return t;
        for (Transform* child : m_transform.children()) {
            if (T* t = child->gameObject().getComponentInChildren<T>()) return t;
        }
        return nullptr;
    }

    // Deferred like destroy(): the component stops getting hooks and is gone
    // from getComponent() / scene queries at once (onDestroy() runs now, if it
    // started), and is freed at the end of the frame. So a component may remove
    // itself or a sibling from inside a hook.
    void removeComponent(Component* component);
    // Includes components removed this frame (Component::isRemoved()).
    const std::vector<std::unique_ptr<Component>>& components() const { return m_components; }

private:
    friend class Scene;
    void attachComponent(std::unique_ptr<Component> component, std::type_index type);

    std::string m_name;
    uint64_t m_id = 0;
    Transform m_transform;
    std::vector<std::unique_ptr<Component>> m_components;
    Scene* m_scene = nullptr;
    bool m_active = true;
    bool m_started = false;
    bool m_destroyed = false;
    bool m_tornDown = false;      // onDestroy() has run (Scene::flushDestroyed)
    bool m_queuedStart = false;   // in Scene::m_unstarted (it or a component still needs start())
    bool m_hasRemoved = false;    // in Scene::m_removals (components to free at the end of the frame)
    uint8_t m_componentsNoHook = 0; // HookBits no component of this object overrides
};

} // namespace ve
