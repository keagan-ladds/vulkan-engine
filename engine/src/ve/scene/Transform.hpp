#pragma once

// =============================================================================
// Transform - position / rotation / scale with a parent-child hierarchy.
//
// Local values are relative to the parent; world values are absolute. The
// world matrix is cached and recomputed lazily when something changed.
// Conventions: right-handed, +Y up, objects look down -Z (forward()).
// =============================================================================

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <vector>

namespace ve {

class GameObject;

class Transform {
public:
    explicit Transform(GameObject& owner) : m_owner(&owner) {}
    ~Transform();

    Transform(const Transform&) = delete;
    Transform& operator=(const Transform&) = delete;

    // --- Local (relative to parent) -----------------------------------------
    const glm::vec3& localPosition() const { return m_position; }
    const glm::quat& localRotation() const { return m_rotation; }
    const glm::vec3& localScale() const { return m_scale; }
    void setLocalPosition(const glm::vec3& p);
    void setLocalRotation(const glm::quat& r);
    void setLocalScale(const glm::vec3& s);
    void setLocalScale(float s) { setLocalScale(glm::vec3(s)); }
    glm::mat4 localMatrix() const;

    // --- World ------------------------------------------------------------------
    glm::vec3 position() const;
    glm::quat rotation() const;
    void setPosition(const glm::vec3& worldPosition);
    void setRotation(const glm::quat& worldRotation);
    const glm::mat4& worldMatrix() const;

    // Euler angles in degrees (pitch = X, yaw = Y, roll = Z), local.
    void setLocalEuler(const glm::vec3& degrees);
    glm::vec3 localEuler() const;

    glm::vec3 forward() const { return rotation() * glm::vec3(0, 0, -1); }
    glm::vec3 right() const { return rotation() * glm::vec3(1, 0, 0); }
    glm::vec3 up() const { return rotation() * glm::vec3(0, 1, 0); }

    // --- Helpers ----------------------------------------------------------------
    void translate(const glm::vec3& delta, bool worldSpace = true);
    void rotate(const glm::vec3& axis, float radians, bool worldSpace = false);
    // Rotates so forward() points at `target`.
    void lookAt(const glm::vec3& target, const glm::vec3& worldUp = glm::vec3(0, 1, 0));
    glm::vec3 transformPoint(const glm::vec3& local) const;
    glm::vec3 inverseTransformPoint(const glm::vec3& world) const;

    // --- Hierarchy --------------------------------------------------------------
    Transform* parent() const { return m_parent; }
    const std::vector<Transform*>& children() const { return m_children; }
    // keepWorld = true keeps the object where it is in the world.
    void setParent(Transform* parent, bool keepWorld = true);
    GameObject& gameObject() const { return *m_owner; }

private:
    friend class Scene;
    void markDirty();

    GameObject* m_owner;
    Transform* m_parent = nullptr;
    std::vector<Transform*> m_children;
    glm::vec3 m_position{0.0f};
    glm::quat m_rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 m_scale{1.0f};
    mutable glm::mat4 m_world{1.0f};
    mutable bool m_dirty = true;
    // Batched static MeshRenderers on this object (Scene::setStaticEntry).
    // Only while there are any does becoming dirty notify the scene, so the
    // renderer moves their instances; every other transform pays one compare.
    uint32_t m_staticEntries = 0;
};

} // namespace ve
