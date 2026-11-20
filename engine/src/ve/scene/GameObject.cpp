#include "ve/scene/GameObject.hpp"

#include "ve/core/Engine.hpp"
#include "ve/scene/ObjectRef.hpp"

#include "ve/scene/Ids.hpp"
#include "ve/scene/Scene.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <random>

namespace ve {

Transform& Component::transform() const
{
    return m_owner->transform();
}

Scene& detail::mainScene()
{
    return Engine::get().scene();
}

bool Component::isActive() const
{
    return enabled && !m_removed && m_owner && m_owner->activeInHierarchy() && !m_owner->isDestroyed();
}

uint64_t newObjectId()
{
    static std::mt19937_64 rng{std::random_device{}()};
    uint64_t id = 0;
    while (id == 0) id = rng();
    return id;
}

std::string idToHex(uint64_t id)
{
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(id));
    return buf;
}

uint64_t idFromHex(std::string_view text)
{
    const std::string s(text); // (strtoull needs a terminator)
    errno = 0;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s.c_str(), &end, 16);
    if (end == s.c_str() || errno == ERANGE) return 0; // not a number / too big
    return static_cast<uint64_t>(v);
}

GameObject::GameObject(std::string name) : m_name(std::move(name)), m_id(newObjectId()), m_transform(*this) {}

GameObject::~GameObject() = default;

bool GameObject::activeInHierarchy() const
{
    if (!m_active) return false;
    const Transform* p = m_transform.parent();
    return p ? p->gameObject().activeInHierarchy() : true;
}

void GameObject::setId(uint64_t id)
{
    const uint64_t old = m_id;
    m_id = id;
    if (m_scene) m_scene->reindex(this, old);
}

void GameObject::destroy()
{
    if (m_destroyed) return;
    m_destroyed = true;
    for (Transform* child : m_transform.children()) child->gameObject().destroy();
    if (m_scene) m_scene->m_hasDestroyed = true;
}

void GameObject::setParent(GameObject* parent, bool keepWorld)
{
    m_transform.setParent(parent ? &parent->transform() : nullptr, keepWorld);
}

void GameObject::attachComponent(std::unique_ptr<Component> component, std::type_index type)
{
    // Serials only need to be unique, not random. Atomic so that building an
    // object off the main thread (before spawning it) stays safe.
    static std::atomic<uint64_t> s_nextSerial{1};
    component->m_owner = this;
    component->m_type = type;
    component->m_serial = s_nextSerial.fetch_add(1, std::memory_order_relaxed);
    m_componentsNoHook = 0; // the new component's hooks are unknown until it runs
    Component* raw = component.get();
    m_components.push_back(std::move(component));
    if (m_scene) {
        m_scene->registerComponent(raw);
        m_scene->queueStart(this); // its start() runs in the next Scene::startPending
    }
}

void GameObject::removeComponent(Component* component)
{
    auto it = std::find_if(m_components.begin(), m_components.end(), [&](const auto& c) { return c.get() == component; });
    if (it == m_components.end() || component->m_removed) return;
    component->m_removed = true;
    if (m_scene) m_scene->unregisterComponent(component);
    // Out of components() / getComponent() at once. A hook may be running on
    // this (or a sibling) component right now, so the scene keeps it alive
    // until the end of the frame (Scene::flushDestroyed).
    std::unique_ptr<Component> owned = std::move(*it);
    m_components.erase(it);
    // Like Scene::destroyNow: only a started component gets onDestroy().
    if (component->m_started) component->onDestroy(); // may add/remove other components
    if (m_scene) m_scene->m_removedComponents.push_back(std::move(owned));
    // (not spawned yet: no hooks can be running, `owned` frees it here)
}

} // namespace ve
