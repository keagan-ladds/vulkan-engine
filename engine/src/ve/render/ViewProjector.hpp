#pragma once

// =============================================================================
// ViewProjector - world <-> screen for a camera (pixels in ImGui display
// coordinates). Used by anything that draws 3D things as 2D overlays: the
// editor's gizmo and icons, debug labels, navmesh debug text.
//
//   const auto view = ve::ViewProjector::from(renderer.frameData().camera, displaySize);
//   if (auto s = view.project(worldPos)) list->AddText({s->x, s->y}, color, "here");
// =============================================================================

#include "ve/render/FrameData.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <optional>

namespace ve {

/// World <-> screen conversions for editor overlays (the camera's matrices and the display size).
struct ViewProjector {
    glm::mat4 viewProj{1.0f};     ///< World to clip.
    glm::mat4 invViewProj{1.0f};  ///< Clip to world.
    glm::vec3 eye{0.0f};  ///< Camera position.
    glm::vec2 size{1.0f}; ///< display size (pixels)
    float tanHalfFov = 0.6f;  ///< tan(vertical fov / 2).

    /// From the frame's camera and the display size in pixels.
    static ViewProjector from(const CameraData& cam, glm::vec2 displaySize)
    {
        ViewProjector v;
        v.viewProj = cam.viewProj;
        v.invViewProj = glm::inverse(cam.viewProj);
        v.eye = cam.position;
        v.size = glm::max(displaySize, glm::vec2(1.0f));
        v.tanHalfFov = std::tan(cam.fovY * 0.5f);
        return v;
    }

    /// Screen position, or nothing when behind the camera.
    std::optional<glm::vec2> project(const glm::vec3& p) const
    {
        const glm::vec4 c = viewProj * glm::vec4(p, 1.0f);
        if (c.w <= 1e-4f) return std::nullopt;
        const glm::vec2 ndc = glm::vec2(c) / c.w;
        return (ndc * 0.5f + 0.5f) * size; // Vulkan NDC: +y is down, like the screen
    }

    /// World-space ray through a screen point.
    void ray(glm::vec2 screen, glm::vec3& origin, glm::vec3& dir) const
    {
        const glm::vec2 ndc = screen / size * 2.0f - 1.0f;
        const glm::vec4 w = invViewProj * glm::vec4(ndc, 0.5f, 1.0f); // reversed-Z: any depth in (0,1] is in front
        origin = eye;
        dir = glm::normalize(glm::vec3(w) / w.w - eye);
    }

    /// World length that covers `pixels` on screen at point p.
    float worldSize(const glm::vec3& p, float pixels) const
    {
        const float dist = std::max(glm::length(p - eye), 0.01f);
        return dist * tanHalfFov * 2.0f * pixels / size.y;
    }
};

} // namespace ve
