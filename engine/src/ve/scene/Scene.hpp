#pragma once

// =============================================================================
// Scene - owns all GameObjects and drives their lifecycle.
//
//   auto& player = scene.spawn<Player>(args...);   // any GameObject subclass
//   auto& empty  = scene.spawn("Empty");            // plain GameObject
//   scene.forEach<PointLight>([](PointLight& l) { l.intensity *= 0.5f; });
//   GameObject* boss = scene.find("Boss");
// =============================================================================

#include "ve/scene/GameObject.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace ve {

class Camera;
class MeshRenderer;
class Model;

/// Owns the GameObjects of a world (or of a loading screen) and runs their hooks
/// each frame. The game scene is Engine::scene(); objects are added with spawn<T>()
/// and removed with GameObject::destroy(). Main thread only.
class Scene {
public:
    Scene() = default;
    ~Scene();

    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    /// Creates an object of type T. Safe to call from inside update(): the
    /// object becomes part of the update loop (and gets start()) next frame.
    template <class T = GameObject, class... Args>
    T& spawn(Args&&... args)
    {
        auto obj = std::make_unique<T>(std::forward<Args>(args)...);
        T& ref = *obj;
        adopt(std::move(obj));
        return ref;
    }

    /// Spawns the node hierarchy of an imported model (glTF) as GameObjects
    /// with MeshRenderers. With staticColliders, every mesh node also gets a
    /// static Rigidbody using the exact triangle mesh (buildings, ruins...).
    /// Returns the root. (Defined in assets/ModelInstantiate.cpp: it builds
    /// anim and physics components, which the scene module can't include.)
    GameObject& instantiate(const Model& model, GameObject* parent = nullptr, bool staticColliders = false);

    /// The first live object with this name (including ones spawned this frame), or null. O(n): cache the result or an id.
    GameObject* find(const std::string& name) const;
    /// O(1). An id is a safe way to remember an object across frames: unlike a
    /// pointer, it can't dangle once the object is destroyed (findById -> null).
    GameObject* findById(uint64_t id) const;
    /// First live object of class T (or a subclass), e.g. the scene's Weather.
    template <class T>
    T* findFirst() const
    {
        for (const auto* list : {&m_objects, &m_pending}) {
            for (const auto& o : *list) {
                if (o->isDestroyed()) continue;
                if (auto* t = dynamic_cast<T*>(o.get())) return t;
            }
        }
        return nullptr;
    }
    /// Live objects with this tag, including ones spawned this frame (like find()).
    std::vector<GameObject*> findAllWithTag(const std::string& tag) const;
    /// The same without allocating: fn(GameObject&) for each.
    template <class F>
    void forEachWithTag(std::string_view tag, F&& fn) const
    {
        for (const auto* list : {&m_objects, &m_pending}) {
            for (size_t i = 0; i < list->size(); ++i) { // (fn may spawn: index loop)
                GameObject* o = (*list)[i].get();
                if (!o->isDestroyed() && o->tag == tag) fn(*o);
            }
        }
    }
    /// Every object that has joined the update loop (objects spawned this frame are added at the start of the next).
    const std::vector<std::unique_ptr<GameObject>>& objects() const { return m_objects; }
    /// Number of objects, including ones spawned this frame.
    size_t objectCount() const { return m_objects.size() + m_pending.size(); }

    /// Iterates all registered components of type T, including subclasses of T
    /// (a `class FlickerLight : public PointLight` is seen as a PointLight).
    template <class T, class F>
    void forEach(F&& fn) const
    {
        for (const Bucket* b : bucketsFor(typeid(T), [](Component* c) { return dynamic_cast<T*>(c) != nullptr; })) {
            // Index loop: fn may add or remove components (bucket order isn't kept).
            for (size_t i = 0; i < b->items.size(); ++i) fn(static_cast<T&>(*b->items[i]));
        }
    }

    /// Highest-priority active Camera (see Camera::priority).
    Camera* mainCamera() const;

    /// Persistent objects created from a level / scene file (save games record
    /// which of them are gone); SceneSerializer::loadJson adds them.
    const std::vector<uint64_t>& levelObjects() const { return m_levelObjects; }
    /// Records a level object's id (SceneSerializer does this while loading).
    void addLevelObject(uint64_t id) { m_levelObjects.push_back(id); }

    // --- Static renderers (MeshRenderer::isStatic) -------------------------------------
    /// The renderer batches static renderers into GPU instance groups once
    /// (render/StaticBatcher) instead of gathering them every frame. Instead of
    /// scanning them, it learns what changed from here: SceneGatherer drains
    /// this every frame. The scene fills it from its hooks: a static renderer
    /// starting or calling refresh(), a Transform with batched renderers
    /// becoming dirty (itself or through a parent), setActive / destroy of an
    /// object above static renderers, and unregistering (removeComponent,
    /// destroy, clear).
    struct StaticChanges {
        std::vector<MeshRenderer*> dirty; ///< to re-check (null: unregistered since it was queued)
        std::vector<uint32_t> released;   ///< batch entries of static renderers that left the scene
        uint32_t entries = 0;             ///< static renderers that have a batch entry now
        uint32_t known = 0;               ///< static renderers seen so far (still registered)
        /// The renderer keeps a weak_ptr to notice when this scene is gone.
        std::shared_ptr<const int> alive = std::make_shared<const int>(0);
    };
    /// The static-renderer change queue (renderer internals: SceneGatherer drains it).
    StaticChanges& staticChanges() { return m_static; }
    /// Queue a renderer for a re-check (static, or not static any more but still batched).
    void queueStaticRenderer(MeshRenderer& renderer);
    /// MeshRenderer::refresh(): the same, if its object is in a scene yet.
    static void refreshStatic(MeshRenderer& renderer);
    /// The static renderers on `o` (and with `recursive`, below it) need a re-check.
    void staticObjectChanged(GameObject& o, bool recursive);
    /// SceneGatherer: takes the queued renderers (swapped into `out`, cleared first).
    void takeStaticDirty(std::vector<MeshRenderer*>& out);
    /// SceneGatherer: a renderer got / lost its batch entry (kNoStaticEntry = none).
    void setStaticEntry(MeshRenderer& renderer, uint32_t entry);
    /// SceneGatherer: the batcher was cleared (static batching turned off):
    /// forget every entry. queueAllStatic(): one scan queueing every static
    /// renderer (static batching turned on, or the renderer starts batching
    /// this scene).
    void resetStaticEntries();
    /// Queues every static renderer for a re-check (see resetStaticEntries).
    void queueAllStatic();

    // --- Engine internal -----------------------------------------------------------
    /// budgetMs > 0 (loading screens): stop starting objects once that much
    /// time has passed; the rest start on later calls (startsDeferred()).
    void startPending(float budgetMs = 0.0f);
    /// The last startPending() stopped early (time budget, or starts held): objects still wait to start.
    bool startsDeferred() const { return m_startsDeferred; }
    /// While held (a counter: hold / release pairs), new objects join the
    /// scene (find() sees them) but don't start or update: a scene file being
    /// instantiated over several frames starts all at once, like a normal load.
    void holdStarts() { ++m_startHolds; }
    /// Undoes one holdStarts(); when none remain, waiting objects start on the next startPending().
    void releaseStarts() { if (m_startHolds > 0) --m_startHolds; }
    /// Whether starts are currently held.
    bool startsHeld() const { return m_startHolds > 0; }
    /// Runs fixedUpdate on every active object and component (Engine: per fixed step).
    void fixedUpdate(float dt);
    /// editorOnly: only objects with runInEditor (and their components) update.
    void update(float dt, bool editorOnly = false);
    /// Runs lateUpdate after update(); `editorOnly` as in update().
    void lateUpdate(float dt, bool editorOnly = false);
    /// Runs onGui on every active object and component (ImGui frame).
    void onGui();
    /// Calls onDestroy and frees the objects destroyed this frame (end of frame).
    void flushDestroyed();
    /// Destroys every object at once (onDestroy runs), e.g. before loading another level.
    void clear();

    /// Adds a component to the per-type registry forEach() uses (attachComponent does it).
    void registerComponent(Component* c);
    /// Removes a component from the per-type registry.
    void unregisterComponent(Component* c);
    void queueStart(GameObject* o); ///< o or one of its components needs start()
    template <class Hook>
    /// Calls `hook` on o's started, enabled components (remembering when none implements `bit`).
    static void callComponents(GameObject& o, uint8_t bit, Hook&& hook);

private:
    std::vector<uint64_t> m_levelObjects;
    friend class GameObject;
    /// Components registered under one exact type, in no particular order
    /// (unregistering swaps the last one into the gap).
    struct Bucket {
        std::type_index type;
        std::vector<Component*> items;
    };
    /// Buckets whose type is T or derives from it; cached per queried type and
    /// rebuilt only when a new component type shows up (registry generation).
    struct Query {
        uint64_t generation = 0;
        std::vector<const Bucket*> buckets;
    };
    template <class Matches>
    const std::vector<const Bucket*>& bucketsFor(std::type_index type, Matches&& matches) const
    {
        Query& q = m_queries[type];
        if (q.generation != m_generation) {
            q.buckets.clear();
            for (const auto& b : m_buckets) {
                if (!b->items.empty() && (b->type == type || matches(b->items.front()))) q.buckets.push_back(b.get());
            }
            q.generation = m_generation;
        }
        return q.buckets;
    }

    void adopt(std::unique_ptr<GameObject> obj);
    void destroyNow(GameObject* obj);
    void flushRemovedComponents();
    /// Runs `fn` on every active object that may implement `hook` (itself or a component).
    template <class Fn>
    void forActive(uint8_t hook, bool editorOnly, Fn&& fn);

    void reindex(GameObject* obj, uint64_t oldId); ///< GameObject::setId

    std::vector<std::unique_ptr<GameObject>> m_objects;
    std::vector<std::unique_ptr<GameObject>> m_pending;
    /// Objects that (or whose components) still need start(): startPending
    /// walks only these instead of every object. Inactive ones stay queued.
    std::vector<GameObject*> m_unstarted, m_startScratch;
    bool m_startsDeferred = false;
    int m_startHolds = 0;
    std::vector<std::unique_ptr<Component>> m_removedComponents; ///< removeComponent: freed at the end of the frame
    std::unordered_map<uint64_t, GameObject*> m_byId;
    std::vector<std::unique_ptr<Bucket>> m_buckets; ///< stable addresses (cached in queries)
    std::unordered_map<std::type_index, Bucket*> m_bucketByType;
    mutable std::unordered_map<std::type_index, Query> m_queries;
    uint64_t m_generation = 1; ///< bumped when a bucket goes from empty to non-empty
    bool m_hasDestroyed = false;
    StaticChanges m_static;
};

} // namespace ve
