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

/// A GameObject's position, rotation and scale, relative to its parent, and its
/// place in the hierarchy. World values are computed (and cached) from the parents.
/// Every GameObject has exactly one (GameObject::transform()).
class Transform {
public:
    /// Made by its GameObject; not created on its own.
    explicit Transform(GameObject& owner) : m_owner(&owner) {}
    ~Transform();

    Transform(const Transform&) = delete;
    Transform& operator=(const Transform&) = delete;

    // --- Local (relative to parent) -----------------------------------------
    /// Position relative to the parent (the world position for a root).
    const glm::vec3& localPosition() const { return m_position; }
    /// Rotation relative to the parent.
    const glm::quat& localRotation() const { return m_rotation; }
    /// Scale relative to the parent (1 = unscaled).
    const glm::vec3& localScale() const { return m_scale; }
    /// Sets the position relative to the parent.
    void setLocalPosition(const glm::vec3& p);
    /// Sets the rotation relative to the parent.
    void setLocalRotation(const glm::quat& r);
    /// Sets the scale relative to the parent (per axis).
    void setLocalScale(const glm::vec3& s);
    /// Sets a uniform scale relative to the parent.
    void setLocalScale(float s) { setLocalScale(glm::vec3(s)); }
    /// Parent-relative transform as a matrix (translation * rotation * scale).
    glm::mat4 localMatrix() const;

    // --- World ------------------------------------------------------------------
    /// Position in world space (metres).
    glm::vec3 position() const;
    /// Rotation in world space.
    glm::quat rotation() const;
    /// Moves the object to a world-space position (whatever its parent).
    void setPosition(const glm::vec3& worldPosition);
    /// Turns the object to a world-space rotation (whatever its parent).
    void setRotation(const glm::quat& worldRotation);
    /// Local-to-world matrix (cached; recomputed after a change here or in a parent).
    const glm::mat4& worldMatrix() const;

    /// Euler angles in degrees (pitch = X, yaw = Y, roll = Z), local.
    void setLocalEuler(const glm::vec3& degrees);
    /// Local rotation as Euler angles in degrees (pitch = X, yaw = Y, roll = Z).
    glm::vec3 localEuler() const;

    /// World-space forward direction: -Z (the way cameras and lights look).
    glm::vec3 forward() const { return rotation() * glm::vec3(0, 0, -1); }
    /// World-space right direction: +X.
    glm::vec3 right() const { return rotation() * glm::vec3(1, 0, 0); }
    /// World-space up direction: +Y.
    glm::vec3 up() const { return rotation() * glm::vec3(0, 1, 0); }

    // --- Helpers ----------------------------------------------------------------
    /// Moves the object by `delta`.
    /// @param worldSpace true: `delta` is in world axes; false: in the object's own (rotated) axes.
    void translate(const glm::vec3& delta, bool worldSpace = true);
    /// Rotates the object around `axis` by `radians`.
    /// @param worldSpace true: `axis` is a world axis; false: one of the object's own axes.
    void rotate(const glm::vec3& axis, float radians, bool worldSpace = false);
    /// Rotates so forward() points at `target`.
    void lookAt(const glm::vec3& target, const glm::vec3& worldUp = glm::vec3(0, 1, 0));
    /// Converts a point from this object's local space to world space.
    glm::vec3 transformPoint(const glm::vec3& local) const;
    /// Converts a world-space point into this object's local space.
    glm::vec3 inverseTransformPoint(const glm::vec3& world) const;

    // --- Hierarchy --------------------------------------------------------------
    /// The parent's transform, or null for a root object.
    Transform* parent() const { return m_parent; }
    /// The direct children's transforms, in the order they were attached.
    const std::vector<Transform*>& children() const { return m_children; }
    /// keepWorld = true keeps the object where it is in the world.
    void setParent(Transform* parent, bool keepWorld = true);
    /// The GameObject this transform belongs to.
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
    /// Batched static MeshRenderers on this object (Scene::setStaticEntry).
    /// Only while there are any does becoming dirty notify the scene, so the
    /// renderer moves their instances; every other transform pays one compare.
    uint32_t m_staticEntries = 0;
};

} // namespace ve
