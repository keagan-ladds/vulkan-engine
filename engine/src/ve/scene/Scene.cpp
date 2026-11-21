#include "ve/scene/Scene.hpp"

#include "ve/scene/Components.hpp"

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
    queueStart(obj.get());
    m_pending.push_back(std::move(obj));
}

void Scene::queueStart(GameObject* o)
{
    if (o->m_queuedStart) return;
    o->m_queuedStart = true;
    m_unstarted.push_back(o);
}

void Scene::registerComponent(Component* c)
{
    if (auto* mr = dynamic_cast<MeshRenderer*>(c); mr && mr->isStatic) queueStaticRenderer(*mr);
    Bucket*& bucket = m_bucketByType[c->m_type];
    if (!bucket) bucket = m_buckets.emplace_back(std::make_unique<Bucket>(Bucket{c->m_type, {}})).get();
    // Queries skip empty buckets (they can't test the type), so re-check them.
    if (bucket->items.empty()) ++m_generation;
    c->m_bucketIndex = static_cast<uint32_t>(bucket->items.size());
    bucket->items.push_back(c);
}

void Scene::unregisterComponent(Component* c)
{
    if (auto* mr = dynamic_cast<MeshRenderer*>(c)) {
        // Leaving the scene: the renderer frees its batch entry next frame,
        // and the queue must not keep a pointer that is about to dangle.
        if (mr->m_staticQueued != MeshRenderer::kNoStaticEntry) {
            m_static.dirty[mr->m_staticQueued] = nullptr;
            mr->m_staticQueued = MeshRenderer::kNoStaticEntry;
        }
        if (const uint32_t entry = mr->m_staticEntry; entry != MeshRenderer::kNoStaticEntry) {
            setStaticEntry(*mr, MeshRenderer::kNoStaticEntry);
            m_static.released.push_back(entry);
        }
        if (mr->m_staticKnown) {
            mr->m_staticKnown = false;
            --m_static.known;
        }
    }
    // O(1): the last item takes the gap. Nothing relies on bucket order
    // (forEach users that care sort, e.g. the renderer's lights).
    auto it = m_bucketByType.find(c->m_type);
    if (it == m_bucketByType.end()) return;
    std::vector<Component*>& items = it->second->items;
    const uint32_t i = c->m_bucketIndex;
    if (i >= items.size() || items[i] != c) return; // not registered
    items[i] = items.back();
    items[i]->m_bucketIndex = i;
    items.pop_back();
}

// -----------------------------------------------------------------------------
// Static renderers (see StaticChanges in Scene.hpp)
// -----------------------------------------------------------------------------

void Scene::queueStaticRenderer(MeshRenderer& mr)
{
    if (mr.isRemoved() || (!mr.isStatic && mr.m_staticEntry == MeshRenderer::kNoStaticEntry)) return;
    if (!mr.m_staticKnown) {
        mr.m_staticKnown = true;
        ++m_static.known;
    }
    if (mr.m_staticQueued != MeshRenderer::kNoStaticEntry) return;
    mr.m_staticQueued = static_cast<uint32_t>(m_static.dirty.size());
    m_static.dirty.push_back(&mr);
}

void Scene::refreshStatic(MeshRenderer& mr)
{
    if (mr.m_owner && mr.m_owner->m_scene) mr.m_owner->m_scene->queueStaticRenderer(mr);
}

void Scene::staticObjectChanged(GameObject& o, bool recursive)
{
    if (m_static.known == 0) return; // no static renderers: nothing to tell
    for (const auto& c : o.m_components) {
        if (auto* mr = dynamic_cast<MeshRenderer*>(c.get())) queueStaticRenderer(*mr);
    }
    if (!recursive) return;
    for (Transform* child : o.transform().children()) staticObjectChanged(child->gameObject(), true);
}

void Scene::takeStaticDirty(std::vector<MeshRenderer*>& out)
{
    out.clear();
    out.swap(m_static.dirty);
    for (MeshRenderer* mr : out) {
        if (mr) mr->m_staticQueued = MeshRenderer::kNoStaticEntry;
    }
}

void Scene::setStaticEntry(MeshRenderer& mr, uint32_t entry)
{
    const bool had = mr.m_staticEntry != MeshRenderer::kNoStaticEntry;
    const bool has = entry != MeshRenderer::kNoStaticEntry;
    mr.m_staticEntry = entry;
    if (had == has) return;
    // The Transform reports moves only while it has batched renderers.
    mr.transform().m_staticEntries += has ? 1 : -1;
    m_static.entries += has ? 1 : -1;
}

void Scene::resetStaticEntries()
{
    forEach<MeshRenderer>([&](MeshRenderer& mr) { setStaticEntry(mr, MeshRenderer::kNoStaticEntry); });
    m_static.released.clear();
}

void Scene::queueAllStatic()
{
    forEach<MeshRenderer>([&](MeshRenderer& mr) {
        if (mr.isStatic) queueStaticRenderer(mr);
    });
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
    forEachWithTag(tag, [&](GameObject& o) { result.push_back(&o); });
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
namespace {

// The object's components as raw pointers, taken before running hooks: a hook
// may remove itself or a sibling (removeComponent takes it out of the list at
// once, which shifts the vector) or add components (they start next frame).
// Removed ones stay alive until the end of the frame, so the pointers stay
// valid; skip those with isRemoved(). No heap allocation for <= 16 components.
class ComponentSnapshot {
public:
    explicit ComponentSnapshot(const std::vector<std::unique_ptr<Component>>& list) : m_size(list.size())
    {
        Component** out = m_inline;
        if (m_size > std::size(m_inline)) {
            m_heap.resize(m_size);
            out = m_heap.data();
        }
        for (size_t i = 0; i < m_size; ++i) out[i] = list[i].get();
    }
    Component* const* begin() const { return m_heap.empty() ? m_inline : m_heap.data(); }
    Component* const* end() const { return begin() + m_size; }

private:
    Component* m_inline[16];
    std::vector<Component*> m_heap;
    size_t m_size;
};

} // namespace

template <class Hook>
void Scene::callComponents(GameObject& o, uint8_t bit, Hook&& hook)
{
    if (o.m_componentsNoHook & bit) return;
    uint8_t none = bit;
    for (Component* c : ComponentSnapshot(o.m_components)) {
        if (c->m_removed) continue;
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

    // Only objects queued by adopt() / attachComponent() (or still waiting from
    // earlier frames: inactive, or with a disabled component) are visited.
    // Anything queued during this loop (start() adding components) lands in
    // the fresh m_unstarted and is handled next frame.
    std::vector<GameObject*>& work = m_startScratch;
    work.clear();
    work.swap(m_unstarted);
    for (size_t w = 0; w < work.size(); ++w) {
        GameObject* o = work[w];
        o->m_queuedStart = false;
        if (o->m_destroyed) continue;
        if (!o->activeInHierarchy()) {
            queueStart(o); // starts once it's active
            continue;
        }
        if (!o->m_started) {
            o->m_started = true;
            o->start();
        }
        // Components added at any point get start() before their first update.
        // (Ones added by these start() calls queue the object for next frame.)
        for (Component* c : ComponentSnapshot(o->m_components)) {
            if (c->m_started || c->m_removed) continue;
            if (c->enabled) {
                c->m_started = true;
                c->start();
            } else {
                queueStart(o); // starts when enabled
            }
        }
    }
    work.clear();
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
    // onDestroy() may add components (repeat until none is left) or remove
    // siblings (they got their onDestroy() in removeComponent). Marking each
    // as removed means it's handled once.
    for (bool again = true; again;) {
        again = false;
        for (Component* c : ComponentSnapshot(o->m_components)) {
            if (c->m_removed) continue;
            c->m_removed = true;
            again = true;
            unregisterComponent(c);
            if (c->m_started) c->onDestroy();
        }
    }
    if (o->m_started) o->onDestroy();
    o->m_transform.setParent(nullptr, false);
}

void Scene::flushRemovedComponents()
{
    // Removed components are freed here, when no hook can be running on them.
    // (Swap first: a destructor that removes another component appends to the
    // fresh list, freed next frame.)
    std::vector<std::unique_ptr<Component>> dead;
    dead.swap(m_removedComponents);
    dead.clear();
}

void Scene::flushDestroyed()
{
    if (!m_removedComponents.empty()) flushRemovedComponents();
    if (!m_hasDestroyed) return;
    m_hasDestroyed = false;
    // Index loops: onDestroy() may spawn (appends to m_pending) or destroy
    // more objects. Only objects whose onDestroy() ran are freed; anything
    // destroyed during the sweep sets m_hasDestroyed and goes next frame.
    auto tearDown = [&](std::vector<std::unique_ptr<GameObject>>& list) {
        for (size_t i = 0; i < list.size(); ++i) {
            GameObject* o = list[i].get();
            if (o->m_destroyed && !o->m_tornDown) {
                o->m_tornDown = true;
                destroyNow(o);
            }
        }
    };
    auto erase = [&](std::vector<std::unique_ptr<GameObject>>& list) {
        std::erase_if(list, [&](const auto& o) {
            if (!o->m_tornDown) return false;
            if (auto it = m_byId.find(o->id()); it != m_byId.end() && it->second == o.get()) m_byId.erase(it);
            return true;
        });
    };
    tearDown(m_objects);
    tearDown(m_pending);
    // The start queue forgets torn-down objects before they're freed (an
    // onDestroy() above may also have removed components of live objects:
    // those wait in m_removedComponents until next frame).
    std::erase_if(m_unstarted, [](const GameObject* o) { return o->m_tornDown; });
    // Components removed by those onDestroy() calls go before their owners.
    if (!m_removedComponents.empty()) flushRemovedComponents();
    erase(m_objects);
    erase(m_pending);
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
    m_unstarted.clear();
    m_removedComponents.clear();
    m_buckets.clear();
    m_bucketByType.clear();
    m_queries.clear();
    ++m_generation;
}

} // namespace ve
