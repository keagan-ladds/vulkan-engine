#include "ve/scene/GameObject.hpp"

#include "ve/scene/Scene.hpp"

#include <algorithm>
#include <random>

namespace ve {

Transform& Component::transform() const
{
    return m_owner->transform();
}

bool Component::isActive() const
{
    return enabled && m_owner && m_owner->activeInHierarchy() && !m_owner->isDestroyed();
}

namespace {
uint64_t newObjectId()
{
    static std::mt19937_64 rng{std::random_device{}()};
    uint64_t id = 0;
    while (id == 0) id = rng();
    return id;
}
} // namespace

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
    component->m_owner = this;
    component->m_type = type;
    m_componentsNoHook = 0; // the new component's hooks are unknown until it runs
    Component* raw = component.get();
    m_components.push_back(std::move(component));
    if (m_scene) m_scene->registerComponent(raw);
}

void GameObject::removeComponent(Component* component)
{
    auto find = [&] {
        return std::find_if(m_components.begin(), m_components.end(), [&](const auto& c) { return c.get() == component; });
    };
    if (find() == m_components.end()) return;
    if (m_scene) m_scene->unregisterComponent(component);
    component->onDestroy(); // may add/remove other components: look it up again afterwards
    if (auto it = find(); it != m_components.end()) m_components.erase(it);
}

} // namespace ve
