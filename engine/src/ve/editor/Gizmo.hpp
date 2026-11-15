#pragma once

// =============================================================================
// Editor viewport helpers, drawn with ImGui over the 3D view:
//   ViewProjector  world <-> screen (pixels in ImGui display coordinates)
//   DebugDraw      3D lines, boxes, spheres, circles (no depth test)
//   Gizmo          translate / rotate / scale handles for one Transform
// =============================================================================

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <optional>

struct ImDrawList;

namespace ve {

class Transform;
struct CameraData;

struct ViewProjector {
    glm::mat4 viewProj{1.0f}, invViewProj{1.0f};
    glm::vec3 eye{0.0f};
    glm::vec2 size{1.0f}; // display size (pixels)
    float tanHalfFov = 0.6f;

    static ViewProjector from(const CameraData& cam, glm::vec2 displaySize);
    // Screen position, or nothing when behind the camera.
    std::optional<glm::vec2> project(const glm::vec3& p) const;
    // World-space ray through a screen point.
    void ray(glm::vec2 screen, glm::vec3& origin, glm::vec3& dir) const;
    // World length that covers `pixels` on screen at point p.
    float worldSize(const glm::vec3& p, float pixels) const;
};

class DebugDraw {
public:
    DebugDraw(ImDrawList* list, const ViewProjector& view) : m_list(list), m_view(view) {}
    void line(const glm::vec3& a, const glm::vec3& b, uint32_t color, float thickness = 1.5f);
    void box(const glm::mat4& transform, const glm::vec3& halfExtents, uint32_t color);
    void aabb(const glm::vec3& min, const glm::vec3& max, uint32_t color);
    void circle(const glm::vec3& center, const glm::vec3& axis, float radius, uint32_t color, int segments = 48);
    void sphere(const glm::vec3& center, float radius, uint32_t color);
    void cylinder(const glm::mat4& transform, float radius, float y0, float y1, uint32_t color);

private:
    ImDrawList* m_list;
    const ViewProjector& m_view;
};

enum class GizmoMode { Translate, Rotate, Scale };

class Gizmo {
public:
    struct Options {
        GizmoMode mode = GizmoMode::Translate;
        bool local = false;     // axes follow the object's rotation (scale always uses local axes)
        bool snap = false;
        float snapMove = 0.5f;  // m
        float snapAngle = 15.0f; // degrees
        float snapScale = 0.1f;
    };
    enum class Event { None, Began, Changed, Ended };

    // Draws the handles and handles the mouse. Call every frame while an object is selected.
    Event manipulate(ImDrawList* list, const ViewProjector& view, Transform& target, const Options& options);
    bool hovered() const { return m_hover >= 0; }
    bool active() const { return m_active >= 0; }

private:
    int m_hover = -1;  // handle under the mouse
    int m_active = -1; // handle being dragged
    glm::vec3 m_startPos{0.0f}, m_startScale{1.0f}, m_startHit{0.0f};
    glm::quat m_startRot{1, 0, 0, 0};
    glm::vec3 m_axis{0.0f}, m_planeNormal{0.0f};
    float m_startParam = 0.0f;
    glm::vec2 m_startMouse{0.0f};
};

} // namespace ve
