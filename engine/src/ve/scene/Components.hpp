#pragma once

// =============================================================================
// Built-in components the renderer understands. They are plain data: add
// them to a GameObject and the renderer picks them up automatically.
// =============================================================================

#include "ve/scene/GameObject.hpp"

#include <memory>
#include <string>
#include <vector>

namespace ve {

class Mesh;
class Material;
class Texture;

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

/// A light that shines in a cone: torches, headlights, stage and street lamps.
/// It shines along transform().forward() (-Z), from the object's position, up
/// to `range` metres, fully bright within innerAngle of the axis and fading
/// to nothing at outerAngle. Otherwise a PointLight (colour, intensity,
/// range, shadows): it counts towards the point-light limits, and a shadowed
/// spot only renders the cube-map faces its cone touches.
///   auto& beam = car.addComponent<ve::SpotLight>();   // on a child placed at the lamp
///   beam.range = 40; beam.outerAngle = 28; beam.innerAngle = 18; beam.intensity = 60;
///
/// Optional extras for a less perfect beam (an old torch, a cheap lens):
///   beam.cookie = "textures/torch_cookie.png"; // mask projected across the cone
///   beam.chromaticAberration = 0.06f;           // red fringe outside, blue inside
class SpotLight : public PointLight {
public:
    SpotLight() { range = 20.0f; intensity = 30.0f; color = {1.0f, 0.95f, 0.85f}; }
    float innerAngle = 20.0f; ///< Degrees from the axis: full brightness inside.
    float outerAngle = 30.0f; ///< Degrees from the axis: the cone's edge (dark beyond). At most 89.
    /// Asset path of a mask ("cookie") projected across the beam: its RGB
    /// multiplies the light. The image's centre is the beam's axis and its
    /// inscribed circle reaches the outer angle; image up = the light's up
    /// (+Y). Rings, a dark spot from the bulb, smudges: an unfocused torch.
    /// Empty = keep whatever cookieTexture code set (none: a clean cone).
    /// Load it with the image's TextureSettings (use a *_mask name or a
    /// .texture file for linear data). Changing it from code: call reload().
    std::string cookie;
    std::shared_ptr<Texture> cookieTexture; ///< The loaded mask (set from code instead of `cookie` if you like); null = none.
    /// Lens colour fringing, 0..0.3: red spreads this fraction wider than
    /// green and blue this much narrower, so the cone's edge (and the
    /// cookie's rings) get a red outer and a blue inner fringe. 0 = off.
    float chromaticAberration = 0.0f;

    void start() override { reload(); } ///< (fields set after addComponent)
    /// Loads `cookie` if it changed since the last load (a cleared path drops the texture).
    void reload();

private:
    std::string m_loadedCookie; ///< the path cookieTexture came from
};

} // namespace ve
