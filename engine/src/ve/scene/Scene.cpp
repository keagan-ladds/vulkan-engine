#include "ve/scene/Scene.hpp"

#include "ve/assets/Model.hpp"
#include "ve/physics/Physics.hpp"
#include "ve/scene/Components.hpp"

#include <glm/gtx/matrix_decompose.hpp>

#include <algorithm>

namespace ve {

Scene::~Scene()
{
    clear();
}

void Scene::adopt(std::unique_ptr<GameObject> obj)
{
    obj->m_scene = this;
    for (const auto& c : obj->m_components) registerComponent(c.get());
    m_pending.push_back(std::move(obj));
}

void Scene::registerComponent(Component* c)
{
    m_registry[c->m_type].push_back(c);
}

void Scene::unregisterComponent(Component* c)
{
    auto it = m_registry.find(c->m_type);
    if (it != m_registry.end()) std::erase(it->second, c);
}

GameObject* Scene::find(const std::string& name) const
{
    for (const auto& o : m_objects) {
        if (!o->m_destroyed && o->name() == name) return o.get();
    }
    for (const auto& o : m_pending) {
        if (!o->m_destroyed && o->name() == name) return o.get();
    }
    return nullptr;
}

std::vector<GameObject*> Scene::findAllWithTag(const std::string& tag) const
{
    std::vector<GameObject*> result;
    for (const auto& o : m_objects) {
        if (!o->m_destroyed && o->tag == tag) result.push_back(o.get());
    }
    return result;
}

Camera* Scene::mainCamera() const
{
    Camera* best = nullptr;
    forEach<Camera>([&](Camera& c) {
        if (c.isActive() && (!best || c.priority > best->priority)) best = &c;
    });
    return best;
}

template <class Fn>
void Scene::forActive(Fn&& fn)
{
    // Index loop: objects spawned during iteration go to m_pending, so this
    // vector does not change size while we iterate.
    for (size_t i = 0; i < m_objects.size(); ++i) {
        GameObject* o = m_objects[i].get();
        if (o->m_destroyed || !o->activeInHierarchy()) continue;
        if (!o->m_started) {
            o->m_started = true;
            o->start();
        }
        fn(*o);
    }
}

void Scene::startPending()
{
    // Move newly spawned objects into the main list (start() may spawn more,
    // which then wait for the next frame).
    std::vector<std::unique_ptr<GameObject>> fresh = std::move(m_pending);
    m_pending.clear();
    for (auto& o : fresh) m_objects.push_back(std::move(o));

    for (auto& o : m_objects) {
        if (o->m_destroyed || !o->activeInHierarchy()) continue;
        if (!o->m_started) {
            o->m_started = true;
            o->start();
        }
        // Components added at any point get start() before their first update.
        for (size_t i = 0; i < o->m_components.size(); ++i) {
            Component* c = o->m_components[i].get();
            if (!c->m_started && c->enabled) {
                c->m_started = true;
                c->start();
            }
        }
    }
}

void Scene::fixedUpdate(float dt)
{
    forActive([dt](GameObject& o) {
        o.fixedUpdate(dt);
        for (size_t i = 0; i < o.m_components.size(); ++i) {
            Component* c = o.m_components[i].get();
            if (c->enabled && c->m_started) c->fixedUpdate(dt);
        }
    });
}

void Scene::update(float dt)
{
    forActive([dt](GameObject& o) {
        o.update(dt);
        for (size_t i = 0; i < o.m_components.size(); ++i) {
            Component* c = o.m_components[i].get();
            if (c->enabled && c->m_started) c->update(dt);
        }
    });
}

void Scene::lateUpdate(float dt)
{
    forActive([dt](GameObject& o) {
        o.lateUpdate(dt);
        for (size_t i = 0; i < o.m_components.size(); ++i) {
            Component* c = o.m_components[i].get();
            if (c->enabled && c->m_started) c->lateUpdate(dt);
        }
    });
}

void Scene::onGui()
{
    forActive([](GameObject& o) {
        o.onGui();
        for (size_t i = 0; i < o.m_components.size(); ++i) {
            Component* c = o.m_components[i].get();
            if (c->enabled && c->m_started) c->onGui();
        }
    });
}

void Scene::destroyNow(GameObject* o)
{
    for (auto& c : o->m_components) {
        unregisterComponent(c.get());
        if (c->m_started) c->onDestroy();
    }
    if (o->m_started) o->onDestroy();
    o->m_transform.setParent(nullptr, false);
}

void Scene::flushDestroyed()
{
    if (!m_hasDestroyed) return;
    m_hasDestroyed = false;
    auto sweep = [&](std::vector<std::unique_ptr<GameObject>>& list) {
        for (auto& o : list) {
            if (o->m_destroyed) destroyNow(o.get());
        }
        std::erase_if(list, [](const auto& o) { return o->m_destroyed; });
    };
    sweep(m_objects);
    sweep(m_pending);
}

void Scene::clear()
{
    for (auto& o : m_objects) o->m_destroyed = true;
    for (auto& o : m_pending) o->m_destroyed = true;
    m_hasDestroyed = true;
    flushDestroyed();
    m_registry.clear();
}

GameObject& Scene::instantiate(const Model& model, GameObject* parent, bool staticColliders)
{
    GameObject& root = spawn(model.name.empty() ? std::string("Model") : model.name);
    if (parent) root.setParent(parent, false);

    // Recursively create one GameObject per glTF node.
    std::function<void(int, GameObject&)> build = [&](int nodeIndex, GameObject& parentObj) {
        const ModelNode& node = model.nodes[nodeIndex];
        GameObject& go = spawn(node.name.empty() ? std::string("Node") : node.name);
        go.setParent(&parentObj, false);

        glm::vec3 scale, translation, skew;
        glm::vec4 perspective;
        glm::quat rotation;
        glm::decompose(node.local, scale, rotation, translation, skew, perspective);
        go.transform().setLocalPosition(translation);
        go.transform().setLocalRotation(rotation);
        go.transform().setLocalScale(scale);

        if (node.mesh >= 0) {
            go.addComponent<MeshRenderer>(model.meshes[node.mesh], model.meshMaterials[node.mesh]);
            if (staticColliders) {
                if (model.meshColliders.size() != model.meshes.size()) model.meshColliders.resize(model.meshes.size());
                Collider& c = model.meshColliders[node.mesh];
                if (!c.valid()) c = Collider::mesh(model.meshData[node.mesh]);
                go.addComponent<Rigidbody>(c, BodyType::Static);
            }
        }
        for (int child : node.children) build(child, go);
    };
    for (int r : model.roots) build(r, root);
    return root;
}

} // namespace ve
