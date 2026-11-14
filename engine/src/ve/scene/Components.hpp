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

// The renderer draws from the active Camera with the highest priority.
class Camera : public Component {
public:
    float fovDegrees = 65.0f; // vertical field of view
    float nearPlane = 0.1f;
    float farPlane = 3000.0f;
    int priority = 0;
};

// Draws a Mesh. materials[i] is used by submeshes whose materialSlot == i.
class MeshRenderer : public Component {
public:
    MeshRenderer() = default;
    MeshRenderer(std::shared_ptr<Mesh> m, std::shared_ptr<Material> mat)
        : mesh(std::move(m))
    {
        if (mat) materials.push_back(std::move(mat));
    }
    MeshRenderer(std::shared_ptr<Mesh> m, std::vector<std::shared_ptr<Material>> mats)
        : mesh(std::move(m)), materials(std::move(mats))
    {
    }

    Material* material(uint32_t slot) const
    {
        if (materials.empty()) return nullptr;
        return materials[slot < materials.size() ? slot : 0].get();
    }

    std::shared_ptr<Mesh> mesh;
    std::vector<std::shared_ptr<Material>> materials;
    bool castShadows = true;
    bool visible = true;
};

// Infinitely distant light (sun, moon). Shines along transform().forward().
// The brightest `RendererSettings::maxShadowedDirectionalLights` shadowed
// lights get cascaded shadow maps.
class DirectionalLight : public Component {
public:
    glm::vec3 color{1.0f, 0.96f, 0.9f};
    float intensity = 3.0f;
    bool castShadows = true;
};

// Omni light with a finite range. The closest
// `RendererSettings::maxShadowedPointLights` shadowed lights get cube shadow maps.
class PointLight : public Component {
public:
    glm::vec3 color{1.0f, 0.7f, 0.4f};
    float intensity = 10.0f;
    float range = 10.0f;
    bool castShadows = false;
    float shadowBias = 0.02f; // in units of range (fraction)
};

} // namespace ve
