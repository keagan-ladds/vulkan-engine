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

/// Per-frame hooks the Scene dispatches; used as bits in "not overridden" masks.
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

/// Shortcuts to engine systems, available in both GameObject and Component.
/// (All of them are Engine::get()'s systems: only valid while the Engine exists.)
class EngineAccess {
public:
    static Engine& engine();            ///< The Engine (Engine::get()).
    static Input& input();              ///< Keyboard, mouse, gamepads and actions.
    static const Time& time();          ///< Frame time: deltaTime, time, frameCount...
    static Renderer& renderer();        ///< The renderer (materials, settings, render passes).
    static AssetManager& assets();      ///< Loading textures, models, sounds, materials.
    static AudioSystem& audio();        ///< Sounds, buses, music.
    static PhysicsWorld& physics();     ///< Raycasts, overlaps, layers, the physics settings.
    static net::Network& net();         ///< Multiplayer session (offline until host / join).
};

/// The lifecycle and physics hooks GameObject and Component share. Override
/// the ones you need; see the hook order at the top of this file.
class Behaviour : public EngineAccess {
public:
    virtual ~Behaviour() = default;

    /// Called once, before the first update, at the start of the frame after
    /// the object (or component) was added. Other objects spawned in the same
    /// frame exist by then, so look things up here, not in the constructor.
    virtual void start() {}
    /// Called every frame (not while the game is paused in the editor, unless
    /// GameObject::runInEditor). @param dt seconds since the last frame, scaled by Time::timeScale.
    virtual void update([[maybe_unused]] float dt) { m_noHook |= kHookUpdate; }
    /// Called at a fixed rate (EngineConfig::fixedTimestep), zero or more times
    /// per frame, before update(): physics-driven movement goes here.
    /// @param dt the fixed timestep in seconds.
    virtual void fixedUpdate([[maybe_unused]] float dt) { m_noHook |= kHookFixedUpdate; }
    /// Called every frame after every update(): follow cameras, things that
    /// read where others moved to. @param dt as in update().
    virtual void lateUpdate([[maybe_unused]] float dt) { m_noHook |= kHookLateUpdate; }
    /// Called every frame while ImGui is open for drawing: debug windows,
    /// ImGui-based UI (game UI proper is ve::ui, ve/ui/Gui.hpp).
    virtual void onGui() { m_noHook |= kHookGui; }
    /// Called once when the object is destroyed (end of the frame destroy() was
    /// called in) or the component removed, if start() ran. Release what
    /// start() acquired here.
    virtual void onDestroy() {}
    /// Two bodies started touching (ve/physics/Physics.hpp). Sent during the
    /// fixed update to the GameObjects on both sides of the contact and all
    /// their components.
    virtual void onCollisionEnter(const Collision&) {}
    /// The bodies are still touching (every fixed step while they do).
    virtual void onCollisionStay(const Collision&) {}
    /// The bodies stopped touching.
    virtual void onCollisionExit(const Collision&) {}
    /// Another object's collider entered a trigger collider (Collider::isTrigger)
    /// on this object, or this object entered `other`'s trigger.
    virtual void onTriggerEnter(GameObject& /*other*/) {}
    /// `other` left the trigger (or was destroyed while inside).
    virtual void onTriggerExit(GameObject& /*other*/) {}

private:
    friend class Scene;
    uint8_t m_noHook = 0; ///< HookBits whose base (empty) version ran: not overridden
};

/// A reusable piece of behaviour or data attached to a GameObject
/// (GameObject::addComponent). Gets the same hooks as the object, after it.
class Component : public Behaviour {
public:
    /// The object this component is attached to.
    GameObject& gameObject() const { return *m_owner; }
    /// The owner's transform (shortcut for gameObject().transform()).
    Transform& transform() const;
    /// Enabled and the owner is active in the hierarchy.
    bool isActive() const;

    /// A disabled component gets no hooks (start() waits until it's enabled).
    bool enabled = true;
    /// removeComponent() was called: it's out of components(), gets no more
    /// hooks, and the scene frees it at the end of the frame.
    bool isRemoved() const { return m_removed; }
    /// Saved in scene files (set by the editor / scene loader). Components a
    /// GameObject class creates for itself stay false.
    bool persistent = false;
    /// Unique for this run and never reused (0 until attached to an object).
    /// ComponentRef (ve/scene/ObjectRef.hpp) uses it to tell a remembered
    /// component apart from a new one the allocator put at the same address.
    uint64_t serial() const { return m_serial; }

private:
    friend class GameObject;
    friend class Scene;
    GameObject* m_owner = nullptr;
    uint64_t m_serial = 0;
    bool m_started = false;
    bool m_removed = false;
    uint32_t m_bucketIndex = 0; ///< position in the Scene's registry bucket (O(1) unregister)
    std::type_index m_type = typeid(Component);
};

/// A thing in the scene: a transform, components, and (in subclasses) its own
/// behaviour. Create with Scene::spawn<T>(...); the scene owns it.
class GameObject : public Behaviour {
public:
    /// Objects are made by Scene::spawn (which calls this), not on their own.
    explicit GameObject(std::string name = "GameObject");
    virtual ~GameObject();

    GameObject(const GameObject&) = delete;
    GameObject& operator=(const GameObject&) = delete;

    // Lifecycle and physics hooks: see Behaviour.

    // --- Identity & state -------------------------------------------------------
    /// The object's name (Scene::find looks objects up by it; the editor shows it).
    const std::string& name() const { return m_name; }
    void setName(std::string name) { m_name = std::move(name); } ///< See name().
    /// Position, rotation, scale and parent / children.
    Transform& transform() { return m_transform; }
    const Transform& transform() const { return m_transform; } ///< See transform().
    /// This object's own active flag (it may still be inactive through a parent).
    bool activeSelf() const { return m_active; }
    /// Active and every parent active: only then does it get hooks and render.
    bool activeInHierarchy() const;
    /// Turns the object (and so everything below it) on or off.
    void setActive(bool active);
    /// Destroys the object and its children at the end of the frame
    /// (onDestroy() then runs); until then it still exists, but isDestroyed().
    void destroy();
    /// destroy() was called (the object is freed at the end of the frame).
    bool isDestroyed() const { return m_destroyed; }
    /// The scene that owns the object (valid once spawned).
    Scene& scene() const { return *m_scene; }
    /// Whether the object belongs to `scene` (false until spawned).
    bool inScene(const Scene& scene) const { return m_scene == &scene; }
    std::string tag; ///< Free-form label for gameplay code to test (saved in scene files).

    // --- Editor / scene files ---------------------------------------------------
    /// Unique, random, and stable across saves and scene files (ObjectRef,
    /// save games and networking refer to objects by it).
    uint64_t id() const { return m_id; }
    /// Changes the id (scene loading, networking); the scene's id index follows.
    void setId(uint64_t id);
    bool persistent = false;      ///< Saved in scene files (objects made in the editor or loaded from a file).
    bool runInEditor = false;     ///< Keeps updating while the editor has the game paused (and while loading).
    bool hideInHierarchy = false; ///< Not listed in the editor's hierarchy (editor internals, helpers).

    /// Parents this object under another.
    /// @param parent the new parent (null = make it a root).
    /// @param keepWorld keep the world position / rotation / scale (else keep
    ///        the local ones, which then become relative to the new parent).
    void setParent(GameObject* parent, bool keepWorld = true);
    /// Marks the MeshRenderers on this object (and, recursive, everything
    /// below it) static or not (MeshRenderer::isStatic: batched on the GPU,
    /// for things that never move) and refreshes them.
    void setStatic(bool isStatic, bool recursive = true);

    // --- Components -------------------------------------------------------------
    /// Creates a component of type T (constructed from `args`) and attaches it.
    /// It starts at the beginning of the next frame (before its first update).
    /// @return the new component (owned by this object).
    template <class T, class... Args>
    T& addComponent(Args&&... args)
    {
        auto c = std::make_unique<T>(std::forward<Args>(args)...);
        T& ref = *c;
        attachComponent(std::move(c), typeid(T));
        return ref;
    }

    /// The first component of type T (or derived from it), or null.
    template <class T>
    T* getComponent() const
    {
        for (const auto& c : m_components) {
            if (c->m_removed) continue;
            if (auto* t = dynamic_cast<T*>(c.get())) return t;
        }
        return nullptr;
    }

    /// Like getComponent, on this object then its children, depth first.
    template <class T>
    T* getComponentInChildren() const
    {
        if (T* t = getComponent<T>()) return t;
        for (Transform* child : m_transform.children()) {
            if (T* t = child->gameObject().getComponentInChildren<T>()) return t;
        }
        return nullptr;
    }

    /// Deferred like destroy(): the component stops getting hooks and is gone
    /// from getComponent() / scene queries at once (onDestroy() runs now, if it
    /// started), and is freed at the end of the frame. So a component may remove
    /// itself or a sibling from inside a hook.
    void removeComponent(Component* component);
    /// Is `component` (maybe a remembered pointer) still a live component of
    /// this object? Only compares addresses, so it's safe on a dangling one;
    /// but a new component may have reused the address: to remember a
    /// component across frames use ComponentRef (ve/scene/ObjectRef.hpp).
    bool hasComponent(const Component* component) const
    {
        for (const auto& c : m_components)
            if (c.get() == component) return true;
        return false;
    }
    /// Every component of the object, in the order added (removed ones are gone).
    const std::vector<std::unique_ptr<Component>>& components() const { return m_components; }

private:
    friend class Scene;
    friend class Transform; // reports reparenting (static renderers)
    void attachComponent(std::unique_ptr<Component> component, std::type_index type);

    std::string m_name;
    uint64_t m_id = 0;
    Transform m_transform;
    std::vector<std::unique_ptr<Component>> m_components;
    Scene* m_scene = nullptr;
    bool m_active = true;
    bool m_started = false;
    bool m_destroyed = false;
    bool m_tornDown = false;      ///< onDestroy() has run (Scene::flushDestroyed)
    bool m_queuedStart = false;   ///< in Scene::m_unstarted (it or a component still needs start())
    uint8_t m_componentsNoHook = 0; ///< HookBits no component of this object overrides
};

} // namespace ve
