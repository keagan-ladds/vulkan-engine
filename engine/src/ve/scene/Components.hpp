#pragma once

// =============================================================================
// Built-in components the renderer understands. They are plain data: add
// them to a GameObject and the renderer picks them up automatically.
// =============================================================================

#include "ve/scene/GameObject.hpp"

#include <memory>
#include <vector>

namespace ve {

class Mesh;
class Material;

/// The renderer draws from the active Camera with the highest priority.
class Camera : public Component {
public:
    float fovDegrees = 65.0f; ///< vertical field of view
    float nearPlane = 0.1f;  ///< Near clip distance in metres (> 0; larger = better depth precision).
    float farPlane = 3000.0f;  ///< Far clip distance in metres.
    int priority = 0;  ///< The active camera with the highest priority renders.
};

/// Draws a Mesh. materials[i] is used by submeshes whose materialSlot == i.
///
/// Static renderers (isStatic, "static" in scene files and the editor): for
/// things that never move (buildings, huts, props). The renderer puts them
/// into GPU instance groups once (RendererSettings::staticBatching) instead
/// of gathering, culling and drawing each one every frame. Moving the object
/// (or a parent), setActive, destroy and removeComponent are noticed by
/// themselves (moving is fine, just not free: each move re-uploads the
/// instance). Changing mesh, materials, visible, castShadows, isStatic or
/// enabled of a static renderer from code needs refresh() afterwards (the
/// editor and scene files do that). A static renderer is drawn the normal
/// way while it is skinned or uses a transparent material.
class MeshRenderer : public Component {
public:
    MeshRenderer() = default;
    /// A renderer for `m` with one material (slot 0).
    MeshRenderer(std::shared_ptr<Mesh> m, std::shared_ptr<Material> mat)
        : mesh(std::move(m))
    {
        if (mat) materials.push_back(std::move(mat));
    }
    /// A renderer for `m` with one material per slot (submesh materialSlot indexes `mats`).
    MeshRenderer(std::shared_ptr<Mesh> m, std::vector<std::shared_ptr<Material>> mats)
        : mesh(std::move(m)), materials(std::move(mats))
    {
    }

    /// The material for a submesh's `slot` (slot 0's when out of range; null if none).
    Material* material(uint32_t slot) const
    {
        if (materials.empty()) return nullptr;
        return materials[slot < materials.size() ? slot : 0].get();
    }

    std::shared_ptr<Mesh> mesh;  ///< What to draw (shared between renderers).
    std::vector<std::shared_ptr<Material>> materials;  ///< One per material slot; see material().
    bool castShadows = true;  ///< Draw into shadow maps.
    bool visible = true;  ///< Draw at all (unlike `enabled`, keeps the component's hooks running).
    bool isStatic = false; ///< never moves: batched on the GPU (see above)

    /// A static renderer's fields (or isStatic / enabled) changed: the renderer
    /// re-checks it next frame. Cheap; does nothing for non-static renderers.
    void refresh();
    void start() override { refresh(); } ///< (fields set after addComponent)

    // --- Engine internal (Scene, SceneGatherer) ---------------------------------------
    /// staticEntry() of a renderer that isn't batched.
    static constexpr uint32_t kNoStaticEntry = 0xFFFFFFFFu;
    uint32_t staticEntry() const { return m_staticEntry; } ///< batch entry (StaticBatcher), or kNoStaticEntry
    /// SceneGatherer: records the renderer's batch entry.
    void setStaticEntry(uint32_t entry) { m_staticEntry = entry; }

private:
    friend class Scene;
    uint32_t m_staticEntry = kNoStaticEntry;
    uint32_t m_staticQueued = kNoStaticEntry; ///< index in Scene::StaticChanges::dirty
    bool m_staticKnown = false;               ///< counted in Scene::StaticChanges::known
};

/// Infinitely distant light (sun, moon). Shines along transform().forward().
/// The brightest `RendererSettings::maxShadowedDirectionalLights` shadowed
/// lights get cascaded shadow maps.
class DirectionalLight : public Component {
public:
    glm::vec3 color{1.0f, 0.96f, 0.9f};  ///< Linear RGB colour.
    float intensity = 3.0f;  ///< Illuminance multiplier (the sun: about 3; the moon: ~0.05).
    bool castShadows = true;  ///< Gets cascaded shadow maps (if among the brightest; see above).
};

/// Omni light with a finite range. The closest
/// `RendererSettings::maxShadowedPointLights` shadowed lights get cube shadow maps.
class PointLight : public Component {
public:
    glm::vec3 color{1.0f, 0.7f, 0.4f};  ///< Linear RGB colour.
    float intensity = 10.0f;  ///< Brightness multiplier.
    float range = 10.0f;  ///< Metres; the light fades to zero at this distance.
    bool castShadows = false;  ///< Gets a cube shadow map (if among the closest; see above).
    float shadowBias = 0.02f; ///< in units of range (fraction)
};

} // namespace ve
