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
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace ve {

class Camera;
class Model;

class Scene {
public:
    Scene() = default;
    ~Scene();

    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    // Creates an object of type T. Safe to call from inside update(): the
    // object becomes part of the update loop (and gets start()) next frame.
    template <class T = GameObject, class... Args>
    T& spawn(Args&&... args)
    {
        auto obj = std::make_unique<T>(std::forward<Args>(args)...);
        T& ref = *obj;
        adopt(std::move(obj));
        return ref;
    }

    // Spawns the node hierarchy of an imported model (glTF) as GameObjects
    // with MeshRenderers. With staticColliders, every mesh node also gets a
    // static Rigidbody using the exact triangle mesh (buildings, ruins...).
    // Returns the root.
    GameObject& instantiate(const Model& model, GameObject* parent = nullptr, bool staticColliders = false);

    GameObject* find(const std::string& name) const;
    // O(1). An id is a safe way to remember an object across frames: unlike a
    // pointer, it can't dangle once the object is destroyed (findById -> null).
    GameObject* findById(uint64_t id) const;
    // First live object of class T (or a subclass), e.g. the scene's Weather.
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
    std::vector<GameObject*> findAllWithTag(const std::string& tag) const;
    const std::vector<std::unique_ptr<GameObject>>& objects() const { return m_objects; }
    size_t objectCount() const { return m_objects.size() + m_pending.size(); }

    // Iterates all registered components of type T, including subclasses of T
    // (a `class FlickerLight : public PointLight` is seen as a PointLight).
    template <class T, class F>
    void forEach(F&& fn) const
    {
        for (const Bucket* b : bucketsFor(typeid(T), [](Component* c) { return dynamic_cast<T*>(c) != nullptr; })) {
            for (Component* c : b->items) fn(static_cast<T&>(*c));
        }
    }

    // Highest-priority active Camera (see Camera::priority).
    Camera* mainCamera() const;

    // --- Engine internal -----------------------------------------------------------
    void startPending();
    void fixedUpdate(float dt);
    // editorOnly: only objects with runInEditor (and their components) update.
    void update(float dt, bool editorOnly = false);
    void lateUpdate(float dt, bool editorOnly = false);
    void onGui();
    void flushDestroyed();
    void clear();

    void registerComponent(Component* c);
    void unregisterComponent(Component* c);
    template <class Hook>
    static void callComponents(GameObject& o, uint8_t bit, Hook&& hook);

private:
    friend class GameObject;
    // Components registered under one exact type.
    struct Bucket {
        std::type_index type;
        std::vector<Component*> items;
    };
    // Buckets whose type is T or derives from it; cached per queried type and
    // rebuilt only when a new component type shows up (registry generation).
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
    // Runs `fn` on every active object that may implement `hook` (itself or a component).
    template <class Fn>
    void forActive(uint8_t hook, bool editorOnly, Fn&& fn);

    void reindex(GameObject* obj, uint64_t oldId); // GameObject::setId

    std::vector<std::unique_ptr<GameObject>> m_objects;
    std::vector<std::unique_ptr<GameObject>> m_pending;
    std::unordered_map<uint64_t, GameObject*> m_byId;
    std::vector<std::unique_ptr<Bucket>> m_buckets; // stable addresses (cached in queries)
    std::unordered_map<std::type_index, Bucket*> m_bucketByType;
    mutable std::unordered_map<std::type_index, Query> m_queries;
    uint64_t m_generation = 1; // bumped when a bucket goes from empty to non-empty
    bool m_hasDestroyed = false;
};

} // namespace ve
