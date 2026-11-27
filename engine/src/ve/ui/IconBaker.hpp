#pragma once

// =============================================================================
// Icon baking - a small picture of a mesh for menus (build menus, inventory
// slots, crafting lists).
//
//   auto icon = ve::ui::bakeIcon(renderer, meshData, 256);   // once, at load
//   image.texture = icon;                                    // a ui::Image
//
// The mesh is drawn on its own (no scene): an orthographic view from above
// and to the side that fits its bounds, a sun and a sky / ground ambient, its
// vertex colours times `tint`, on a transparent background, rendered at twice
// the size and filtered down (smooth edges). Load-time only (it waits for
// the GPU).
// =============================================================================

#include "ve/render/Mesh.hpp"

#include <glm/glm.hpp>

#include <memory>

namespace ve {

class Renderer;
class Texture;

namespace ui {

/// How bakeIcon frames and lights a mesh.
struct IconView {
    float yawDegrees = 35.0f;   ///< around the object (0 = looking along -z)
    float pitchDegrees = 28.0f; ///< down from the horizon
    glm::vec3 sunDirection{-0.45f, -0.8f, -0.4f}; ///< towards which light travels
    glm::vec3 sunColor{1.0f, 0.95f, 0.85f};  ///< Sun colour (linear RGB).
    glm::vec3 skyColor{0.55f, 0.6f, 0.7f};  ///< Ambient light from above (linear RGB).
    glm::vec3 groundColor{0.25f, 0.2f, 0.16f};  ///< Ambient light from below (linear RGB).
    glm::vec4 tint{1.0f};  ///< Multiplies the mesh's vertex colours.
    float padding = 0.06f;      ///< fraction of the icon left around the object
};

std::shared_ptr<Texture> bakeIcon(Renderer& renderer, const MeshData& mesh, uint32_t size = 256, const IconView& view = {});

} // namespace ui
} // namespace ve
