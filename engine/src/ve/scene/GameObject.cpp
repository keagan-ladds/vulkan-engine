#include "ve/scene/GameObject.hpp"

#include "ve/scene/Scene.hpp"

#include <algorithm>

namespace ve {

Transform& Component::transform() const
{
    return m_owner->transform();
}

bool Component::isActive() const
{
    return enabled && m_owner && m_owner->activeInHierarchy() && !m_owner->isDestroyed();
}

GameObject::GameObject(std::string name) : m_name(std::move(name)), m_transform(*this) {}

GameObject::~GameObject() = default;

bool GameObject::activeInHierarchy() const
{
    if (!m_active) return false;
    const Transform* p = m_transform.parent();
    return p ? p->gameObject().activeInHierarchy() : true;
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
    Component* raw = component.get();
    m_components.push_back(std::move(component));
    if (m_scene) m_scene->registerComponent(raw);
}

void GameObject::removeComponent(Component* component)
{
    auto it = std::find_if(m_components.begin(), m_components.end(),
                           [&](const auto& c) { return c.get() == component; });
    if (it == m_components.end()) return;
    if (m_scene) m_scene->unregisterComponent(component);
    component->onDestroy();
    m_components.erase(it);
}

} // namespace ve
