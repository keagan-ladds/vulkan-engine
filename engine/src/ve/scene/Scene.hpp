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
    GameObject* findById(uint64_t id) const;
    std::vector<GameObject*> findAllWithTag(const std::string& tag) const;
    const std::vector<std::unique_ptr<GameObject>>& objects() const { return m_objects; }
    size_t objectCount() const { return m_objects.size() + m_pending.size(); }

    // Iterates all registered components of exactly type T (as added with addComponent<T>).
    template <class T, class F>
    void forEach(F&& fn) const
    {
        const auto it = m_registry.find(typeid(T));
        if (it == m_registry.end()) return;
        for (Component* c : it->second) fn(static_cast<T&>(*c));
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

private:
    friend class GameObject;
    void adopt(std::unique_ptr<GameObject> obj);
    void destroyNow(GameObject* obj);
    template <class Fn>
    void forActive(Fn&& fn);

    std::vector<std::unique_ptr<GameObject>> m_objects;
    std::vector<std::unique_ptr<GameObject>> m_pending;
    std::unordered_map<std::type_index, std::vector<Component*>> m_registry;
    bool m_hasDestroyed = false;
};

} // namespace ve
