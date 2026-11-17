#include "ve/scene/Scene.hpp"

#include "ve/anim/Animation.hpp"

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
    m_byId[obj->id()] = obj.get();
    for (const auto& c : obj->m_components) registerComponent(c.get());
    m_pending.push_back(std::move(obj));
}

void Scene::registerComponent(Component* c)
{
    Bucket*& bucket = m_bucketByType[c->m_type];
    if (!bucket) bucket = m_buckets.emplace_back(std::make_unique<Bucket>(Bucket{c->m_type, {}})).get();
    // Queries skip empty buckets (they can't test the type), so re-check them.
    if (bucket->items.empty()) ++m_generation;
    bucket->items.push_back(c);
}

void Scene::unregisterComponent(Component* c)
{
    auto it = m_bucketByType.find(c->m_type);
    if (it != m_bucketByType.end()) std::erase(it->second->items, c);
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

GameObject* Scene::findById(uint64_t id) const
{
    const auto it = m_byId.find(id);
    return it != m_byId.end() && !it->second->m_destroyed ? it->second : nullptr;
}

void Scene::reindex(GameObject* obj, uint64_t oldId)
{
    if (auto it = m_byId.find(oldId); it != m_byId.end() && it->second == obj) m_byId.erase(it);
    m_byId[obj->id()] = obj;
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
void Scene::forActive(uint8_t hook, bool editorOnly, Fn&& fn)
{
    // Index loop: objects spawned during iteration go to m_pending, so this
    // vector does not change size while we iterate.
    for (size_t i = 0; i < m_objects.size(); ++i) {
        GameObject* o = m_objects[i].get();
        if (o->m_destroyed || (editorOnly && !o->runInEditor)) continue;
        // Nothing here implements this hook (most objects: terrain chunks,
        // model nodes, static props): skip without walking the hierarchy.
        if (o->m_started && (o->m_noHook & hook) && (o->m_componentsNoHook & hook)) continue;
        if (!o->activeInHierarchy()) continue;
        if (!o->m_started) {
            o->m_started = true;
            o->start();
        }
        fn(*o);
    }
}

// Calls `hook` on o's started, enabled components, and remembers when none of
// them implements it.
template <class Hook>
void Scene::callComponents(GameObject& o, uint8_t bit, Hook&& hook)
{
    if (o.m_componentsNoHook & bit) return;
    uint8_t none = bit;
    for (size_t i = 0; i < o.m_components.size(); ++i) {
        Component* c = o.m_components[i].get();
        if (c->enabled && c->m_started) hook(*c);
        none &= c->m_noHook; // unknown (not run yet) counts as implemented
    }
    // (Components added during the loop reset the mask in attachComponent.)
    o.m_componentsNoHook |= none;
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
    forActive(kHookFixedUpdate, false, [dt](GameObject& o) {
        if (!(o.m_noHook & kHookFixedUpdate)) o.fixedUpdate(dt);
        callComponents(o, kHookFixedUpdate, [dt](Component& c) { c.fixedUpdate(dt); });
    });
}

void Scene::update(float dt, bool editorOnly)
{
    forActive(kHookUpdate, editorOnly, [dt](GameObject& o) {
        if (!(o.m_noHook & kHookUpdate)) o.update(dt);
        callComponents(o, kHookUpdate, [dt](Component& c) { c.update(dt); });
    });
}

void Scene::lateUpdate(float dt, bool editorOnly)
{
    forActive(kHookLateUpdate, editorOnly, [dt](GameObject& o) {
        if (!(o.m_noHook & kHookLateUpdate)) o.lateUpdate(dt);
        callComponents(o, kHookLateUpdate, [dt](Component& c) { c.lateUpdate(dt); });
    });
}

void Scene::onGui()
{
    forActive(kHookGui, false, [](GameObject& o) {
        if (!(o.m_noHook & kHookGui)) o.onGui();
        callComponents(o, kHookGui, [](Component& c) { c.onGui(); });
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
        // Index loop: onDestroy() may spawn (appends to m_pending) or destroy
        // more objects. Only objects whose onDestroy() ran are freed; anything
        // destroyed during the sweep sets m_hasDestroyed and goes next frame.
        for (size_t i = 0; i < list.size(); ++i) {
            GameObject* o = list[i].get();
            if (o->m_destroyed && !o->m_tornDown) {
                o->m_tornDown = true;
                destroyNow(o);
            }
        }
        std::erase_if(list, [&](const auto& o) {
            if (!o->m_tornDown) return false;
            if (auto it = m_byId.find(o->id()); it != m_byId.end() && it->second == o.get()) m_byId.erase(it);
            return true;
        });
    };
    sweep(m_objects);
    sweep(m_pending);
}

void Scene::clear()
{
    m_levelObjects.clear();
    // onDestroy() hooks may spawn or destroy more objects: repeat until empty.
    for (int pass = 0; pass < 8 && (!m_objects.empty() || !m_pending.empty()); ++pass) {
        for (auto& o : m_objects) o->m_destroyed = true;
        for (auto& o : m_pending) o->m_destroyed = true;
        m_hasDestroyed = true;
        flushDestroyed();
    }
    m_byId.clear();
    m_buckets.clear();
    m_bucketByType.clear();
    m_queries.clear();
    ++m_generation;
}

GameObject& Scene::instantiate(const Model& model, GameObject* parent, bool staticColliders)
{
    GameObject& root = spawn(model.name.empty() ? std::string("Model") : model.name);
    if (parent) root.setParent(parent, false);

    // Recursively create one GameObject per glTF node.
    std::vector<GameObject*> objects(model.nodes.size(), nullptr);
    std::function<void(int, GameObject&)> build = [&](int nodeIndex, GameObject& parentObj) {
        const ModelNode& node = model.nodes[nodeIndex];
        GameObject& go = spawn(node.name.empty() ? std::string("Node") : node.name);
        objects[size_t(nodeIndex)] = &go;
        go.setParent(&parentObj, false);

        glm::vec3 scale, translation, skew;
        glm::vec4 perspective;
        glm::quat rotation;
        glm::decompose(node.local, scale, rotation, translation, skew, perspective);
        go.transform().setLocalPosition(translation);
        go.transform().setLocalRotation(rotation);
        go.transform().setLocalScale(scale);

        if (node.mesh >= 0 && node.skin >= 0) {
            // Bound below, once every joint object exists.
            auto& skinned = go.addComponent<SkinnedMeshRenderer>();
            skinned.materials = model.meshMaterials[node.mesh];
        } else if (node.mesh >= 0) {
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

    // Skeletal animation: skinned meshes follow their joint objects, an Animator plays the clips.
    for (size_t n = 0; n < model.nodes.size(); ++n) {
        const ModelNode& node = model.nodes[n];
        if (node.skin < 0 || !objects[n]) continue;
        const ModelSkin& skin = model.skins[size_t(node.skin)];
        std::vector<GameObject*> joints;
        for (int j : skin.joints) joints.push_back(j >= 0 && size_t(j) < objects.size() ? objects[size_t(j)] : nullptr);
        if (auto* s = objects[n]->getComponent<SkinnedMeshRenderer>())
            s->bind(model.skinnedMeshes[size_t(node.mesh)], joints, skin.inverseBind);
    }
    if (!model.animations.empty()) root.addComponent<Animator>().bind(model.animations, objects);
    return root;
}

} // namespace ve
