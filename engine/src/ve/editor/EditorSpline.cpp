// Editor: spline editing (part of Editor; see Editor.hpp).
//
// Select an object with a Spline, then Spline > "Edit points" (or Tab in the
// viewport). While editing:
//   click a point          select it; drag it along the ground (Shift: up / down)
//   Ctrl + click           add a point: on the curve = inserted there, else after
//                          the selected point (or at the end)
//   Delete                 remove the selected point
//   Esc / Tab              stop editing (the move gizmo comes back)
// "Snap to ground" keeps dragged / added points on the terrain or colliders.
// Every change is one undo step.

#include "ve/editor/Editor.hpp"

#include "ve/core/Engine.hpp"
#include "ve/editor/EditorInternal.hpp"
#include "ve/physics/Physics.hpp"
#include "ve/scene/Scene.hpp"
#include "ve/scene/Spline.hpp"
#include "ve/world/Terrain.hpp"
#include "ve/world/TerrainSpline.hpp"
#include "ve/world/Vegetation.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace ve {

namespace {

constexpr uint32_t kCurveColor = IM_COL32(80, 220, 255, 230);
constexpr uint32_t kPointColor = IM_COL32(80, 220, 255, 255);
constexpr uint32_t kSelectedColor = IM_COL32(255, 220, 40, 255);

/// Ray march + bisection onto the terrain's height function.
std::optional<glm::vec3> rayToTerrain(const Terrain& terrain, const glm::vec3& o, const glm::vec3& d)
{
    auto below = [&](const glm::vec3& p) { return terrain.inBounds(p.x, p.z) && p.y < terrain.height(p.x, p.z); };
    float t0 = 0.0f;
    for (float t = 0.5f; t < 6000.0f; t += std::max(0.5f, t * 0.01f)) {
        if (below(o + d * t)) {
            float a = t0, b = t;
            for (int k = 0; k < 16; ++k) {
                const float m = 0.5f * (a + b);
                (below(o + d * m) ? b : a) = m;
            }
            return o + d * b;
        }
        t0 = t;
    }
    return std::nullopt;
}

} // namespace

std::optional<glm::vec3> Editor::groundUnder(const ViewProjector& view, glm::vec2 mouse) const
{
    glm::vec3 o, d;
    view.ray(mouse, o, d);
    std::optional<glm::vec3> best;
    if (Terrain* terrain = Terrain::find(m_engine->scene())) best = rayToTerrain(*terrain, o, d);
    // Colliders too (roads over bridges, fences along walls), whichever is nearer.
    if (auto hit = m_engine->physics().raycast(o, d, 6000.0f)) {
        const bool terrainBody = hit->surface == "Terrain";
        if (!terrainBody && (!best || hit->distance < glm::length(*best - o))) best = hit->point;
    }
    return best;
}

bool Editor::splineViewport(Spline& spline, const ViewProjector& view, DebugDraw& dd, glm::vec2 mouse)
{
    // The curve (always, while selected) and its points.
    const auto path = spline.path();
    const auto& samples = path->samples();
    for (size_t i = 1; i < samples.size(); ++i) dd.line(samples[i - 1].position, samples[i].position, kCurveColor, 2.0f);
    // What follows the spline: a road's width and falloff, vegetation corridors.
    auto edges = [&](float halfWidth, uint32_t color) {
        if (halfWidth <= 0.0f || samples.size() < 2) return;
        const size_t stride = std::max<size_t>(1, samples.size() / 600); // (long roads: fewer lines)
        glm::vec3 prevL(0.0f), prevR(0.0f);
        for (size_t i = 0; i < samples.size(); i += stride) {
            const auto& s = samples[std::min(i, samples.size() - 1)];
            const glm::vec3 right = glm::normalize(glm::vec3(-s.tangent.z, 0.0f, s.tangent.x) + glm::vec3(1e-6f, 0, 0));
            const glm::vec3 l = s.position - right * halfWidth * s.width, r = s.position + right * halfWidth * s.width;
            if (i > 0) {
                dd.line(prevL, l, color);
                dd.line(prevR, r, color);
            }
            prevL = l;
            prevR = r;
        }
    };
    if (auto* road = dynamic_cast<TerrainSpline*>(&spline.gameObject())) {
        edges(road->width * 0.5f, IM_COL32(255, 170, 60, 220));
        edges(road->width * 0.5f + road->falloff, IM_COL32(255, 170, 60, 90));
    }
    for (const auto& c : spline.gameObject().components())
        if (auto* fe = dynamic_cast<FoliageExclusion*>(c.get()); fe && fe->spline && !fe->isRemoved())
            edges(fe->width * 0.5f, fe->foliage ? IM_COL32(90, 230, 120, 120) : IM_COL32(170, 230, 90, 100));
        else if (auto* tm = dynamic_cast<TerrainMask*>(c.get()); tm && !tm->isRemoved()) {
            edges(tm->width * 0.5f, IM_COL32(200, 120, 255, 160));               // the sub-biome at full strength...
            edges(tm->width * 0.5f + tm->falloff, IM_COL32(200, 120, 255, 70)); // ...and where it fades out
        }
    if (samples.size() >= 2) {
        // Direction of travel at the start.
        const glm::vec3 a = samples[0].position;
        const glm::vec3 b = path->at(std::min(path->length(), view.worldSize(a, 30.0f))).position;
        dd.line(a, b, kSelectedColor, 3.0f);
    }
    ImDrawList* list = ImGui::GetBackgroundDrawList();
    const auto& points = spline.points();
    int hover = -1;
    float hoverDist = 9.0f;
    for (size_t i = 0; i < points.size(); ++i) {
        const auto s = view.project(spline.toWorld(points[i].position));
        if (!s) continue;
        const bool sel = m_splineEdit && int(i) == m_splinePoint;
        list->AddRectFilled(ImVec2(s->x - 4, s->y - 4), ImVec2(s->x + 4, s->y + 4), sel ? kSelectedColor : kPointColor);
        list->AddRect(ImVec2(s->x - 5, s->y - 5), ImVec2(s->x + 5, s->y + 5), IM_COL32(10, 10, 10, 200));
        const float dist = glm::length(*s - mouse);
        if (m_splineEdit && dist < hoverDist) {
            hoverDist = dist;
            hover = int(i);
        }
    }
    if (!m_splineEdit) return false;
    if (m_splinePoint >= int(points.size())) m_splinePoint = int(points.size()) - 1;

    const ImGuiIO& io = ImGui::GetIO();
    GameObject& obj = spline.gameObject();

    // --- Dragging the selected point ---
    if (m_splineDragging) {
        if (!ImGui::IsMouseDown(0)) {
            m_splineDragging = false;
            endEdit("Move spline point");
            return true;
        }
        if (m_splinePoint < 0) return true;
        SplinePoint p = points[size_t(m_splinePoint)];
        glm::vec3 world = spline.toWorld(p.position);
        if (io.KeyShift) {
            // Up / down: screen pixels -> metres at the point's distance.
            world.y = m_splineDragY - (mouse.y - m_splineDragMouse.y) * view.worldSize(world, 1.0f);
        } else if (m_splineSnap) {
            if (auto g = groundUnder(view, mouse)) world = *g;
        } else {
            // Along the horizontal plane through the point.
            glm::vec3 o, d;
            view.ray(mouse, o, d);
            if (std::abs(d.y) > 1e-4f) {
                const float t = (m_splineDragY - o.y) / d.y;
                if (t > 0.0f) world = o + d * t;
            }
        }
        p.position = spline.toLocal(world);
        spline.setPoint(size_t(m_splinePoint), p);
        return true;
    }
    if (io.WantCaptureMouse) return true;

    if (ImGui::IsMouseClicked(0)) {
        if (hover >= 0 && !io.KeyCtrl) {
            m_splinePoint = hover;
            beginEdit(obj); // the whole drag is one undo step
            m_splineDragging = true;
            m_splineDragMouse = mouse;
            m_splineDragY = spline.toWorld(points[size_t(hover)].position).y;
        } else if (io.KeyCtrl) {
            // Add a point: inserted into the segment under the mouse, else after
            // the selected one (or at the end).
            std::optional<glm::vec3> at = groundUnder(view, mouse);
            if (!at || !m_splineSnap) {
                glm::vec3 o, d;
                view.ray(mouse, o, d);
                const float planeY = points.empty() ? (at ? at->y : 0.0f) : spline.toWorld(points.back().position).y;
                if (std::abs(d.y) > 1e-4f && (planeY - o.y) / d.y > 0.0f) at = o + d * ((planeY - o.y) / d.y);
            }
            if (at) {
                size_t index = points.size();
                if (path->samples().size() >= 2) {
                    const auto c = path->closest(*at, view.worldSize(*at, 10.0f), true);
                    if (std::isfinite(c.separation)) index = path->segmentAt(c.distance) + 1;
                    else if (m_splinePoint >= 0) index = size_t(m_splinePoint) + 1;
                }
                beginEdit(obj);
                SplinePoint p;
                p.position = spline.toLocal(*at);
                if (!points.empty()) p.width = points[std::min(index, points.size()) - (index > 0 ? 1 : 0)].width;
                spline.insertPoint(index, p);
                m_splinePoint = int(index);
                endEdit("Add spline point");
            }
        } else {
            m_splinePoint = -1;
        }
    }
    return true;
}

void Editor::removeSelectedSplinePoint(Spline& spline)
{
    if (m_splinePoint < 0 || m_splinePoint >= int(spline.points().size())) return;
    beginEdit(spline.gameObject());
    spline.removePoint(size_t(m_splinePoint));
    m_splinePoint = std::min(m_splinePoint, int(spline.points().size()) - 1);
    endEdit("Remove spline point");
}

void Editor::drawSplineTools(Spline& spline)
{
    ImGui::PushID(&spline);
    if (ImGui::Button(m_splineEdit ? "Stop editing points (Tab)" : "Edit points (Tab)", ImVec2(-1.0f, 0.0f))) {
        m_splineEdit = !m_splineEdit;
        m_splineDragging = false;
    }
    ImGui::Checkbox("Snap to ground", &m_splineSnap);
    if (m_splineEdit)
        ImGui::TextDisabled("Click: select / drag (Shift: height)\nCtrl+click: add (on the curve: insert)\nDelete: remove point");
    ImGui::Text("%zu points, %.1f m", spline.points().size(), spline.path()->length());

    if (m_splinePoint >= 0 && m_splinePoint < int(spline.points().size())) {
        SplinePoint p = spline.points()[size_t(m_splinePoint)];
        ImGui::SeparatorText(("Point " + std::to_string(m_splinePoint)).c_str());
        // Each widget: undo snapshot when grabbed, one undo step when released.
        auto edited = [&](bool changed) {
            if (ImGui::IsItemActivated()) beginEdit(spline.gameObject());
            if (changed) spline.setPoint(size_t(m_splinePoint), p);
            if (ImGui::IsItemDeactivatedAfterEdit()) endEdit("Edit spline point");
        };
        edited(ImGui::DragFloat3("Position (local)", &p.position.x, 0.05f));
        edited(ImGui::DragFloat("Width", &p.width, 0.01f, 0.0f, 100.0f));
        if (ImGui::SmallButton("Remove point")) removeSelectedSplinePoint(spline);
    }
    ImGui::Separator();
    if (ImGui::SmallButton("Reverse")) {
        beginEdit(spline.gameObject());
        spline.reverse();
        if (m_splinePoint >= 0) m_splinePoint = int(spline.points().size()) - 1 - m_splinePoint;
        endEdit("Reverse spline");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Drop all to ground")) {
        if (Terrain* terrain = Terrain::find(m_engine->scene())) {
            beginEdit(spline.gameObject());
            std::vector<SplinePoint> pts = spline.points();
            for (SplinePoint& p : pts) {
                glm::vec3 w = spline.toWorld(p.position);
                if (terrain->inBounds(w.x, w.z)) w.y = terrain->height(w.x, w.z);
                p.position = spline.toLocal(w);
            }
            spline.setPoints(std::move(pts));
            endEdit("Drop spline to ground");
        }
    }
    ImGui::PopID();
}

} // namespace ve
