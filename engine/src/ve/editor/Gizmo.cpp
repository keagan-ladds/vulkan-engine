#include "ve/editor/Gizmo.hpp"

#include "ve/render/FrameData.hpp"
#include "ve/scene/Transform.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ve {

namespace {

constexpr float kHandlePixels = 110.0f; // gizmo size on screen
constexpr float kPickPixels = 8.0f;

uint32_t axisColor(int axis, bool highlight, float alpha = 1.0f)
{
    const auto a = static_cast<int>(alpha * 255.0f);
    if (highlight) return IM_COL32(255, 220, 40, a);
    switch (axis) {
    case 0: return IM_COL32(230, 60, 60, a);
    case 1: return IM_COL32(80, 210, 70, a);
    default: return IM_COL32(70, 120, 240, a);
    }
}

ImVec2 iv(glm::vec2 v) { return ImVec2(v.x, v.y); }

float distToSegment(glm::vec2 p, glm::vec2 a, glm::vec2 b)
{
    const glm::vec2 ab = b - a;
    const float t = glm::clamp(glm::dot(p - a, ab) / std::max(glm::dot(ab, ab), 1e-6f), 0.0f, 1.0f);
    return glm::length(p - (a + ab * t));
}

bool insideQuad(glm::vec2 p, const glm::vec2 q[4])
{
    // Convex polygon test (either winding).
    float sign = 0.0f;
    for (int i = 0; i < 4; ++i) {
        const glm::vec2 e = q[(i + 1) % 4] - q[i];
        const float c = e.x * (p.y - q[i].y) - e.y * (p.x - q[i].x);
        if (std::abs(c) < 1e-6f) continue;
        if (sign == 0.0f) sign = c;
        else if ((c > 0.0f) != (sign > 0.0f)) return false;
    }
    return true;
}

// Parameter along line O + t*A of the point closest to ray Ro + s*D.
float closestParam(const glm::vec3& o, const glm::vec3& a, const glm::vec3& ro, const glm::vec3& d)
{
    const glm::vec3 w0 = o - ro;
    const float b = glm::dot(a, d), dd = glm::dot(a, w0), e = glm::dot(d, w0);
    const float denom = 1.0f - b * b;
    if (std::abs(denom) < 1e-5f) return 0.0f;
    return (b * e - dd) / denom;
}

std::optional<glm::vec3> rayPlane(const glm::vec3& ro, const glm::vec3& d, const glm::vec3& p, const glm::vec3& n)
{
    const float denom = glm::dot(n, d);
    if (std::abs(denom) < 1e-4f) return std::nullopt;
    const float t = glm::dot(p - ro, n) / denom;
    if (t < 0.0f) return std::nullopt;
    return ro + d * t;
}

float snapTo(float v, float step) { return step > 0.0f ? std::round(v / step) * step : v; }

} // namespace

// -----------------------------------------------------------------------------
// DebugDraw
// -----------------------------------------------------------------------------

void DebugDraw::line(const glm::vec3& a, const glm::vec3& b, uint32_t color, float thickness)
{
    // Clip against a plane just in front of the camera so lines through the
    // camera don't flip to the other side of the screen.
    glm::vec4 ca = m_view.viewProj * glm::vec4(a, 1.0f);
    glm::vec4 cb = m_view.viewProj * glm::vec4(b, 1.0f);
    const float nearW = 0.05f;
    if (ca.w < nearW && cb.w < nearW) return;
    if (ca.w < nearW) ca = glm::mix(ca, cb, (nearW - ca.w) / (cb.w - ca.w));
    if (cb.w < nearW) cb = glm::mix(cb, ca, (nearW - cb.w) / (ca.w - cb.w));
    const glm::vec2 sa = (glm::vec2(ca) / ca.w * 0.5f + 0.5f) * m_view.size;
    const glm::vec2 sb = (glm::vec2(cb) / cb.w * 0.5f + 0.5f) * m_view.size;
    m_list->AddLine(iv(sa), iv(sb), color, thickness);
}

void DebugDraw::box(const glm::mat4& m, const glm::vec3& h, uint32_t color)
{
    glm::vec3 c[8];
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 l((i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z);
        c[i] = glm::vec3(m * glm::vec4(l, 1.0f));
    }
    const int e[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& ed : e) line(c[ed[0]], c[ed[1]], color);
}

void DebugDraw::aabb(const glm::vec3& mn, const glm::vec3& mx, uint32_t color)
{
    glm::mat4 m(1.0f);
    m[3] = glm::vec4((mn + mx) * 0.5f, 1.0f);
    box(m, (mx - mn) * 0.5f, color);
}

void DebugDraw::circle(const glm::vec3& center, const glm::vec3& axis, float radius, uint32_t color, int segments)
{
    const glm::vec3 n = glm::normalize(axis);
    const glm::vec3 t = glm::normalize(std::abs(n.y) < 0.99f ? glm::cross(n, glm::vec3(0, 1, 0)) : glm::cross(n, glm::vec3(1, 0, 0)));
    const glm::vec3 b = glm::cross(n, t);
    glm::vec3 prev = center + t * radius;
    for (int i = 1; i <= segments; ++i) {
        const float a = 2.0f * std::numbers::pi_v<float> * float(i) / float(segments);
        const glm::vec3 p = center + (t * std::cos(a) + b * std::sin(a)) * radius;
        line(prev, p, color);
        prev = p;
    }
}

void DebugDraw::sphere(const glm::vec3& c, float r, uint32_t color)
{
    circle(c, {1, 0, 0}, r, color);
    circle(c, {0, 1, 0}, r, color);
    circle(c, {0, 0, 1}, r, color);
}

void DebugDraw::cylinder(const glm::mat4& m, float r, float y0, float y1, uint32_t color)
{
    const glm::vec3 axis = glm::normalize(glm::vec3(m[1]));
    const float sx = glm::length(glm::vec3(m[0]));
    const glm::vec3 base = glm::vec3(m * glm::vec4(0, y0, 0, 1)), top = glm::vec3(m * glm::vec4(0, y1, 0, 1));
    circle(base, axis, r * sx, color);
    circle(top, axis, r * sx, color);
    for (int i = 0; i < 4; ++i) {
        const float a = std::numbers::pi_v<float> * 0.5f * float(i);
        const glm::vec3 l(std::cos(a) * r, 0.0f, std::sin(a) * r);
        line(glm::vec3(m * glm::vec4(l + glm::vec3(0, y0, 0), 1)), glm::vec3(m * glm::vec4(l + glm::vec3(0, y1, 0), 1)), color);
    }
}

// -----------------------------------------------------------------------------
// Gizmo
// -----------------------------------------------------------------------------

Gizmo::Event Gizmo::manipulate(ImDrawList* list, const ViewProjector& view, Transform& target, const Options& opt)
{
    const ImGuiIO& io = ImGui::GetIO();
    const glm::vec2 mouse(io.MousePos.x, io.MousePos.y);
    const glm::vec3 origin = target.position();
    const auto originScreen = view.project(origin);
    if (!originScreen) {
        m_hover = -1;
        if (m_active >= 0 && !io.MouseDown[0]) m_active = -1;
        return Event::None;
    }
    const float L = view.worldSize(origin, kHandlePixels);
    const glm::quat rot = target.rotation();
    const bool localAxes = opt.local || opt.mode == GizmoMode::Scale;
    glm::vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    if (localAxes) {
        for (auto& a : axes) a = glm::normalize(rot * a);
    }
    glm::vec3 ro, rd;
    view.ray(mouse, ro, rd);
    DebugDraw dd(list, view);

    // ---- Hover (only when not dragging and the mouse isn't over a window) ----
    const bool overUi = ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) || ImGui::IsAnyItemActive();
    if (m_active < 0) {
        m_hover = -1;
        float best = kPickPixels;
        if (!overUi) {
            for (int i = 0; i < 3; ++i) {
                if (opt.mode == GizmoMode::Rotate) {
                    // Ring: distance to the projected circle polyline.
                    const glm::vec3 n = axes[i];
                    const glm::vec3 t = glm::normalize(std::abs(n.y) < 0.99f ? glm::cross(n, glm::vec3(0, 1, 0)) : glm::cross(n, glm::vec3(1, 0, 0)));
                    const glm::vec3 b = glm::cross(n, t);
                    std::optional<glm::vec2> prev;
                    for (int s = 0; s <= 48; ++s) {
                        const float a = 2.0f * std::numbers::pi_v<float> * float(s) / 48.0f;
                        const auto p = view.project(origin + (t * std::cos(a) + b * std::sin(a)) * L);
                        if (p && prev) {
                            const float d = distToSegment(mouse, *prev, *p);
                            if (d < best) {
                                best = d;
                                m_hover = i;
                            }
                        }
                        prev = p;
                    }
                } else {
                    const auto end = view.project(origin + axes[i] * L);
                    if (!end) continue;
                    const float d = distToSegment(mouse, *originScreen, *end);
                    if (d < best) {
                        best = d;
                        m_hover = i;
                    }
                }
            }
            if (opt.mode == GizmoMode::Translate && m_hover < 0) {
                for (int i = 0; i < 3; ++i) { // plane handles: normal axis i
                    const glm::vec3 u = axes[(i + 1) % 3] * L * 0.25f, v = axes[(i + 2) % 3] * L * 0.25f;
                    glm::vec2 q[4] = {};
                    bool ok = true;
                    const glm::vec3 corners[4] = {origin + u * 0.6f + v * 0.6f, origin + u * 1.4f + v * 0.6f,
                                                  origin + u * 1.4f + v * 1.4f, origin + u * 0.6f + v * 1.4f};
                    for (int k = 0; k < 4; ++k) {
                        auto p = view.project(corners[k]);
                        if (!p) ok = false;
                        else q[k] = *p;
                    }
                    if (ok && insideQuad(mouse, q)) m_hover = 3 + i;
                }
            }
            if (opt.mode == GizmoMode::Scale && glm::length(mouse - *originScreen) < 10.0f) m_hover = 3;
        }
    }

    // ---- Begin drag ----
    Event event = Event::None;
    if (m_active < 0 && m_hover >= 0 && ImGui::IsMouseClicked(0) && !overUi) {
        m_active = m_hover;
        m_startPos = origin;
        m_startRot = target.rotation();
        m_startScale = target.localScale();
        m_startMouse = mouse;
        if (opt.mode == GizmoMode::Translate && m_active < 3) {
            m_axis = axes[m_active];
            m_startParam = closestParam(origin, m_axis, ro, rd);
        } else if (opt.mode == GizmoMode::Translate) {
            m_planeNormal = axes[m_active - 3];
            m_startHit = rayPlane(ro, rd, origin, m_planeNormal).value_or(origin);
        } else if (opt.mode == GizmoMode::Rotate) {
            m_axis = axes[m_active];
            m_startHit = rayPlane(ro, rd, origin, m_axis).value_or(origin + glm::vec3(1, 0, 0));
        } else if (m_active < 3) {
            m_axis = axes[m_active];
            m_startParam = closestParam(origin, m_axis, ro, rd);
        }
        event = Event::Began;
    }

    // ---- Drag ----
    if (m_active >= 0) {
        if (!io.MouseDown[0]) {
            m_active = -1;
            return Event::Ended;
        }
        if (opt.mode == GizmoMode::Translate) {
            glm::vec3 delta(0.0f);
            if (m_active < 3) {
                float d = closestParam(m_startPos, m_axis, ro, rd) - m_startParam;
                if (opt.snap) d = snapTo(d, opt.snapMove);
                delta = m_axis * d;
            } else if (auto hit = rayPlane(ro, rd, m_startPos, m_planeNormal)) {
                delta = *hit - m_startHit;
                if (opt.snap) delta = glm::vec3(snapTo(delta.x, opt.snapMove), snapTo(delta.y, opt.snapMove), snapTo(delta.z, opt.snapMove));
            }
            target.setPosition(m_startPos + delta);
        } else if (opt.mode == GizmoMode::Rotate) {
            if (auto hit = rayPlane(ro, rd, m_startPos, m_axis)) {
                const glm::vec3 v0 = m_startHit - m_startPos, v1 = *hit - m_startPos;
                float angle = std::atan2(glm::dot(glm::cross(v0, v1), m_axis), glm::dot(v0, v1));
                if (opt.snap) angle = glm::radians(snapTo(glm::degrees(angle), opt.snapAngle));
                target.setRotation(glm::normalize(glm::angleAxis(angle, m_axis) * m_startRot));
            }
        } else {
            glm::vec3 s = m_startScale;
            if (m_active < 3) {
                float f = 1.0f + (closestParam(m_startPos, m_axis, ro, rd) - m_startParam) / std::max(L, 1e-4f);
                if (opt.snap) f = std::max(snapTo(f * m_startScale[m_active], opt.snapScale), opt.snapScale) / m_startScale[m_active];
                s[m_active] = std::max(m_startScale[m_active] * f, 0.001f);
            } else {
                float f = std::max(1.0f + (mouse.x - m_startMouse.x) / 150.0f, 0.01f);
                if (opt.snap) f = std::max(snapTo(f, opt.snapScale), opt.snapScale);
                s = glm::max(m_startScale * f, glm::vec3(0.001f));
            }
            target.setLocalScale(s);
        }
        if (event == Event::None) event = Event::Changed;
    }

    // ---- Draw ----
    const glm::vec3 o = target.position();
    const float L2 = view.worldSize(o, kHandlePixels);
    for (int i = 0; i < 3; ++i) {
        const bool hl = (m_active >= 0 ? m_active : m_hover) == i;
        const uint32_t col = axisColor(i, hl);
        if (opt.mode == GizmoMode::Rotate) {
            dd.circle(o, axes[i], L2, col, 64);
        } else {
            const glm::vec3 end = o + axes[i] * L2;
            dd.line(o, end, col, hl ? 3.5f : 2.5f);
            const auto e = view.project(end);
            if (e) {
                if (opt.mode == GizmoMode::Translate) list->AddCircleFilled(iv(*e), 5.5f, col);
                else list->AddRectFilled(ImVec2(e->x - 5, e->y - 5), ImVec2(e->x + 5, e->y + 5), col);
            }
        }
    }
    if (opt.mode == GizmoMode::Translate) {
        for (int i = 0; i < 3; ++i) {
            const bool hl = (m_active >= 0 ? m_active : m_hover) == 3 + i;
            const glm::vec3 u = axes[(i + 1) % 3] * L2 * 0.25f, v = axes[(i + 2) % 3] * L2 * 0.25f;
            const glm::vec3 c[4] = {o + u * 0.6f + v * 0.6f, o + u * 1.4f + v * 0.6f, o + u * 1.4f + v * 1.4f, o + u * 0.6f + v * 1.4f};
            ImVec2 q[4];
            bool ok = true;
            for (int k = 0; k < 4; ++k) {
                auto p = view.project(c[k]);
                if (!p) ok = false;
                else q[k] = iv(*p);
            }
            if (ok) list->AddConvexPolyFilled(q, 4, axisColor(i, hl, hl ? 0.8f : 0.45f));
        }
    }
    if (opt.mode == GizmoMode::Scale) {
        const bool hl = (m_active >= 0 ? m_active : m_hover) == 3;
        if (auto c = view.project(o))
            list->AddRectFilled(ImVec2(c->x - 6, c->y - 6), ImVec2(c->x + 6, c->y + 6), hl ? IM_COL32(255, 220, 40, 255) : IM_COL32(230, 230, 230, 230));
    }
    return event;
}

} // namespace ve
