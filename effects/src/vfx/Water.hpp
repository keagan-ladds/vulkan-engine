#pragma once

// =============================================================================
// Water - a flat water plane at the object's height with a transparent shader:
// depth-based colour and soft shores (reads SceneDepth), sky reflections (the
// atmosphere's sky LUT) and rain ripples (the weather's wetness).
// A scene object (registered "Water"): place it and set its size.
// =============================================================================

#include <ve/scene/GameObject.hpp>

#include <glm/glm.hpp>

#include <memory>

namespace ve {
class Material;
class MeshRenderer;
} // namespace ve

namespace vfx {

class Water : public ve::GameObject {
public:
    Water() : GameObject("Water") { runInEditor = true; }
    void start() override;

    glm::vec2 size{640.0f};
    glm::vec4 deepColor{0.05f, 0.09f, 0.06f, 1.0f};
    glm::vec4 shallowColor{0.22f, 0.25f, 0.15f, 1.0f};
    float absorption = 1.2f;     // how quickly colour goes from shallow to deep
    float shoreFoam = 0.5f;
    float rippleScale = 0.35f;
    float rippleStrength = 0.12f;

    void rebuild(); // applies size / material fields (editor edits)

private:
    std::shared_ptr<ve::Material> m_material;
    ve::MeshRenderer* m_renderer = nullptr;
};

} // namespace vfx
