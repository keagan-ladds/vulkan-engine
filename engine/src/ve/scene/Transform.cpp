#include "ve/scene/Transform.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>

namespace ve {

Transform::~Transform()
{
    setParent(nullptr, false);
    for (Transform* c : m_children) c->m_parent = nullptr;
}

void Transform::markDirty()
{
    if (m_dirty) return; // children are already dirty too
    m_dirty = true;
    for (Transform* c : m_children) c->markDirty();
}

void Transform::setLocalPosition(const glm::vec3& p)
{
    m_position = p;
    m_dirty = false; // force propagation even if already dirty
    markDirty();
}

void Transform::setLocalRotation(const glm::quat& r)
{
    m_rotation = glm::normalize(r);
    m_dirty = false;
    markDirty();
}

void Transform::setLocalScale(const glm::vec3& s)
{
    m_scale = s;
    m_dirty = false;
    markDirty();
}

glm::mat4 Transform::localMatrix() const
{
    // T * R * S: scale first, then rotate, then move.
    return glm::translate(glm::mat4(1.0f), m_position) * glm::mat4_cast(m_rotation) *
           glm::scale(glm::mat4(1.0f), m_scale);
}

const glm::mat4& Transform::worldMatrix() const
{
    if (m_dirty) {
        m_world = m_parent ? m_parent->worldMatrix() * localMatrix() : localMatrix();
        m_dirty = false;
    }
    return m_world;
}

glm::vec3 Transform::position() const
{
    return glm::vec3(worldMatrix()[3]);
}

glm::quat Transform::rotation() const
{
    return m_parent ? m_parent->rotation() * m_rotation : m_rotation;
}

void Transform::setPosition(const glm::vec3& world)
{
    setLocalPosition(m_parent ? glm::vec3(glm::inverse(m_parent->worldMatrix()) * glm::vec4(world, 1.0f)) : world);
}

void Transform::setRotation(const glm::quat& world)
{
    setLocalRotation(m_parent ? glm::inverse(m_parent->rotation()) * world : world);
}

void Transform::setLocalEuler(const glm::vec3& degrees)
{
    setLocalRotation(glm::quat(glm::radians(degrees)));
}

glm::vec3 Transform::localEuler() const
{
    return glm::degrees(glm::eulerAngles(m_rotation));
}

void Transform::translate(const glm::vec3& delta, bool worldSpace)
{
    if (worldSpace) setPosition(position() + delta);
    else setLocalPosition(m_position + m_rotation * delta);
}

void Transform::rotate(const glm::vec3& axis, float radians, bool worldSpace)
{
    const glm::quat q = glm::angleAxis(radians, glm::normalize(axis));
    if (worldSpace) setRotation(q * rotation());
    else setLocalRotation(m_rotation * q);
}

void Transform::lookAt(const glm::vec3& target, const glm::vec3& worldUp)
{
    const glm::vec3 dir = target - position();
    if (glm::length(dir) < 1e-6f) return;
    // quatLookAt builds a rotation whose -Z axis points along `dir`.
    const glm::vec3 d = glm::normalize(dir);
    const glm::vec3 up = std::abs(glm::dot(d, glm::normalize(worldUp))) > 0.999f ? glm::vec3(0, 0, 1) : worldUp;
    setRotation(glm::quatLookAt(d, up));
}

glm::vec3 Transform::transformPoint(const glm::vec3& local) const
{
    return glm::vec3(worldMatrix() * glm::vec4(local, 1.0f));
}

glm::vec3 Transform::inverseTransformPoint(const glm::vec3& world) const
{
    return glm::vec3(glm::inverse(worldMatrix()) * glm::vec4(world, 1.0f));
}

void Transform::setParent(Transform* parent, bool keepWorld)
{
    if (parent == m_parent) return;
    const glm::mat4 world = worldMatrix();
    if (m_parent) std::erase(m_parent->m_children, this);
    m_parent = parent;
    if (m_parent) m_parent->m_children.push_back(this);

    if (keepWorld) {
        const glm::mat4 local = m_parent ? glm::inverse(m_parent->worldMatrix()) * world : world;
        // Decompose (assumes no shear).
        m_position = glm::vec3(local[3]);
        m_scale = {glm::length(glm::vec3(local[0])), glm::length(glm::vec3(local[1])), glm::length(glm::vec3(local[2]))};
        const glm::mat3 rot{glm::vec3(local[0]) / m_scale.x, glm::vec3(local[1]) / m_scale.y,
                            glm::vec3(local[2]) / m_scale.z};
        m_rotation = glm::normalize(glm::quat_cast(rot));
    }
    m_dirty = false;
    markDirty();
}

} // namespace ve
