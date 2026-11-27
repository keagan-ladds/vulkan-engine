// The viewport: the editor fly camera, click picking (icons, then the GPU
// PickingPass), viewport icons, selection shapes and the gizmo, and the
// terrain sculpt / paint tools. See Editor.hpp.

#include "ve/editor/Editor.hpp"

#include "ve/audio/Audio.hpp"
#include "ve/core/Engine.hpp"
#include "ve/core/Input.hpp"
#include "ve/editor/EditorInternal.hpp"
#include "ve/editor/PickingPass.hpp"
#include "ve/gameplay/Placement.hpp"
#include "ve/physics/Physics.hpp"
#include "ve/render/Renderer.hpp"
#include "ve/scene/Components.hpp"
#include "ve/scene/Decal.hpp"
#include "ve/scene/Scene.hpp"
#include "ve/scene/Spline.hpp"
#include "ve/world/Terrain.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <functional>

namespace ve {

using namespace editor_internal;

// -----------------------------------------------------------------------------
// Editor camera
// -----------------------------------------------------------------------------

GameObject* Editor::camera() const
{
    return m_camera.get(m_engine->scene());
}

GameObject& Editor::ensureCamera()
{
    if (GameObject* existing = camera()) return *existing;
    GameObject& cam = m_engine->scene().spawn("Editor Camera");
    cam.hideInHierarchy = true;
    cam.runInEditor = true;
    auto& c = cam.addComponent<Camera>();
    c.priority = 1000000;
    c.farPlane = 6000.0f;
    cam.transform().setPosition(m_camPosition);
    m_camera = &cam;
    return cam;
}

void Editor::updateCamera(float dt)
{
    GameObject* cam = camera();
    if (!cam) { // the game cleared the scene (e.g. back to its menu): a new one
        cam = &ensureCamera();
        cam->setActive(true);
    }
    Input& in = m_engine->input();
    const ImGuiIO& io = ImGui::GetIO();

    // Hold the right mouse button in the viewport to look around and fly (WASD, Q/E down/up).
    const bool rmb = in.mouseDown(MouseButton::Right) || (m_looking && ImGui::IsMouseDown(1));
    if (rmb && !m_looking && !io.WantCaptureMouse) {
        m_looking = true;
        in.setMouseCaptured(true);
    } else if (!rmb && m_looking) {
        m_looking = false;
        in.setMouseCaptured(false);
    }
    if (m_looking) {
        const glm::vec2 d = in.mouseDelta() * 0.0025f;
        m_camYaw -= d.x;
        m_camPitch = glm::clamp(m_camPitch - d.y, -1.55f, 1.55f);
        m_camSpeed = glm::clamp(m_camSpeed * std::pow(1.15f, in.wheel()), 0.5f, 500.0f);
    }
    Transform& t = cam->transform();
    t.setRotation(glm::quat(glm::vec3(m_camPitch, m_camYaw, 0.0f)));
    if (m_looking) {
        glm::vec3 move = t.forward() * in.axis(Key::S, Key::W) + t.right() * in.axis(Key::A, Key::D);
        move.y += in.axis(Key::Q, Key::E);
        t.translate(move * m_camSpeed * (in.keyDown(Key::LShift) ? 4.0f : 1.0f) * dt);
    } else if (!io.WantCaptureMouse && in.wheel() != 0.0f) {
        t.translate(t.forward() * in.wheel() * m_camSpeed * 0.25f); // dolly
    }
    m_camPosition = t.position();
}

// -----------------------------------------------------------------------------
// Viewport: icons, selection shapes, gizmo, picking
// -----------------------------------------------------------------------------

void Editor::requestPick(glm::ivec2 pixel)
{
    if (m_picking) m_picking->request(pixel);
}

void Editor::drawViewport()
{
    const ImGuiIO& io = ImGui::GetIO();
    Scene& scene = m_engine->scene();
    const ViewProjector view = ViewProjector::from(m_engine->renderer().frameData().camera, {io.DisplaySize.x, io.DisplaySize.y});
    ImDrawList* list = ImGui::GetBackgroundDrawList();
    DebugDraw dd(list, view);
    const glm::vec2 mouse(io.MousePos.x, io.MousePos.y);

    // --- Icons for things you can't otherwise see or click ---
    // From the component registry (no per-object getComponent calls); an
    // object with several gets the first kind in this list.
    std::vector<Icon>& icons = m_icons;
    icons.clear();
    if (m_showIcons) {
        std::vector<IconCandidate>& cands = m_iconCandidates;
        cands.clear();
        auto add = [&](Component& c, int rank, const char* letter, uint32_t color) { cands.push_back({&c.gameObject(), rank, letter, color}); };
        scene.forEach<PointLight>([&](PointLight& c) { add(c, 0, "L", IM_COL32(255, 210, 80, 230)); });
        scene.forEach<DirectionalLight>([&](DirectionalLight& c) { add(c, 1, "S", IM_COL32(255, 240, 150, 230)); });
        scene.forEach<AudioZone>([&](AudioZone& c) { add(c, 2, "Z", IM_COL32(80, 200, 255, 230)); });
        scene.forEach<AudioSource>([&](AudioSource& c) { add(c, 3, "A", IM_COL32(230, 120, 255, 230)); });
        scene.forEach<Camera>([&](Camera& c) { add(c, 4, "C", IM_COL32(220, 220, 220, 230)); });
        scene.forEach<DecalProjector>([&](DecalProjector& c) { add(c, 4, "D", IM_COL32(120, 255, 200, 230)); });
        for (const auto& o : scene.objects()) // empty markers: cheap checks only
            if (o->persistent && o->components().empty() && o->transform().children().empty())
                cands.push_back({o.get(), 5, "o", IM_COL32(200, 200, 200, 200)});
        std::sort(cands.begin(), cands.end(), [](const IconCandidate& a, const IconCandidate& b) {
            return a.object != b.object ? std::less<>()(a.object, b.object) : a.rank < b.rank;
        });
        for (size_t i = 0; i < cands.size(); ++i) {
            if (i > 0 && cands[i].object == cands[i - 1].object) continue; // a better icon was taken
            GameObject* o = cands[i].object;
            const char* letter = cands[i].letter;
            const uint32_t color = cands[i].color;
            if (o->isDestroyed() || o->hideInHierarchy || !o->activeInHierarchy()) continue;
            if (!m_showCodeObjects && !o->persistent) continue;
            const glm::vec3 p = o->transform().position();
            if (glm::length(p - view.eye) > 120.0f) continue;
            const auto s = view.project(p);
            if (!s || s->x < 0 || s->y < 0 || s->x > view.size.x || s->y > view.size.y) continue;
            const bool sel = o->id() == m_selected.id();
            list->AddCircleFilled(ImVec2(s->x, s->y), 9.0f, sel ? IM_COL32(255, 220, 40, 255) : IM_COL32(20, 20, 20, 170));
            list->AddCircle(ImVec2(s->x, s->y), 9.0f, color, 0, 1.5f);
            list->AddText(ImVec2(s->x - 3.5f, s->y - 7.0f), color, letter);
            icons.push_back({*s, o->id()});
        }
    }

    // --- Selection: bounds, gizmo and component shapes ---
    GameObject* sel = selected();
    if (sel) {
        AABB b;
        if (subtreeBounds(*sel, b)) dd.aabb(b.min, b.max, IM_COL32(255, 200, 40, 200));
        const glm::mat4& m = sel->transform().worldMatrix();
        if (auto* rb = sel->getComponent<Rigidbody>(); rb && rb->shape != ColliderShape::Custom) {
            const uint32_t col = IM_COL32(90, 255, 120, 220);
            const glm::vec3 s = rb->shapeSize;
            const glm::mat4 sm = glm::translate(m, rb->shapeOffset);
            switch (rb->shape) {
            case ColliderShape::Box: dd.box(sm, s * 0.5f, col); break;
            case ColliderShape::Sphere: dd.sphere(glm::vec3(sm[3]), s.x * 0.5f * glm::length(glm::vec3(m[0])), col); break;
            default: dd.cylinder(sm, s.x * 0.5f, -s.y * 0.5f, s.y * 0.5f, col); break;
            }
        }
        if (auto* z = sel->getComponent<AudioZone>()) {
            const uint32_t col = IM_COL32(80, 200, 255, 220);
            switch (z->shape) {
            case AudioZone::Shape::Box: dd.box(m, z->halfExtents, col); break;
            case AudioZone::Shape::Sphere: dd.sphere(glm::vec3(m[3]), z->radius * glm::length(glm::vec3(m[0])), col); break;
            case AudioZone::Shape::Cylinder: dd.cylinder(m, z->radius, 0.0f, z->height, col); break;
            }
        }
        if (auto* st = sel->getComponent<TerrainStamp>()) {
            const glm::vec3 c(m[3]);
            dd.circle(c, {0, 1, 0}, std::max(st->radius, 0.05f), IM_COL32(255, 160, 60, 230), 64);
            dd.circle(c, {0, 1, 0}, st->radius + st->falloff, IM_COL32(255, 160, 60, 110), 64);
            if (!st->biome.empty()) dd.circle(c, {0, 1, 0}, st->biomeRadius + st->biomeFalloff * 0.5f, IM_COL32(120, 255, 140, 160), 64);
        }
        if (auto* spot = sel->getComponent<SpotLight>()) {
            // The cone: its axis, and the outer / inner edges at the range.
            const glm::vec3 apex(m[3]);
            const glm::vec3 axis = glm::normalize(glm::vec3(m * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
            const uint32_t col = IM_COL32(255, 210, 80, 170);
            for (const float angle : {spot->outerAngle, spot->innerAngle}) {
                const float a = glm::radians(std::clamp(angle, 0.0f, 89.0f));
                const glm::vec3 centre = apex + axis * (spot->range * std::cos(a));
                const float radius = spot->range * std::sin(a);
                dd.circle(centre, axis, radius, angle == spot->outerAngle ? col : IM_COL32(255, 210, 80, 90));
                if (angle == spot->outerAngle) {
                    const glm::vec3 side = glm::normalize(glm::abs(axis.y) < 0.99f ? glm::cross(axis, glm::vec3(0, 1, 0)) : glm::cross(axis, glm::vec3(1, 0, 0)));
                    const glm::vec3 up = glm::cross(side, axis);
                    for (const glm::vec3& d : {side, -side, up, -up}) dd.line(apex, centre + d * radius, col);
                }
            }
            dd.line(apex, apex + axis * spot->range, IM_COL32(255, 210, 80, 120));
        } else if (auto* l = sel->getComponent<PointLight>()) {
            dd.sphere(glm::vec3(m[3]), l->range, IM_COL32(255, 210, 80, 120));
        }
        if (sel->getComponent<DecalProjector>()) {
            // The projection box (unit cube scaled by the transform) and its direction (-Z).
            const uint32_t col = IM_COL32(120, 255, 200, 220);
            dd.box(m, glm::vec3(0.5f), col);
            const glm::vec3 c(m[3]), back(m * glm::vec4(0.0f, 0.0f, 0.5f, 1.0f)), front(m * glm::vec4(0.0f, 0.0f, -0.5f, 1.0f));
            dd.line(back, front, col, 2.0f);
            dd.line(c, glm::vec3(m * glm::vec4(0.0f, 0.5f, 0.0f, 1.0f)), IM_COL32(120, 255, 200, 120)); // texture up
        }
        if (auto* a = sel->getComponent<AudioSource>(); a && a->spatial) {
            dd.circle(glm::vec3(m[3]), {0, 1, 0}, a->minDistance, IM_COL32(230, 120, 255, 200));
            dd.circle(glm::vec3(m[3]), {0, 1, 0}, a->maxDistance, IM_COL32(230, 120, 255, 110));
        }

        if (auto* spline = sel->getComponent<Spline>()) {
            // Editing points: they own the mouse (no gizmo, no picking).
            if (splineViewport(*spline, view, dd, mouse)) return;
        } else {
            m_splineEdit = false;
        }
        auto* terrain = dynamic_cast<Terrain*>(sel);
        if (terrain && m_terrainTool >= 0) {
            // Sculpting / painting: the brush owns the mouse (no gizmo, no picking).
            terrainBrush(*terrain, view, dd, mouse);
            return;
        }
        ImDrawList* fg = ImGui::GetBackgroundDrawList();
        const Gizmo::Event ev = m_gizmo.manipulate(fg, view, sel->transform(), m_gizmoOptions);
        // Modular pieces: while moving, pull the object onto a nearby snap point of another.
        if (m_snapToPoints && m_gizmoOptions.mode == GizmoMode::Translate && (ev == Gizmo::Event::Changed || ev == Gizmo::Event::Ended)) {
            const auto points = placement::snapPointsOf(*sel);
            if (auto s = placement::findSnap(m_engine->scene(), points, sel->transform().rotation(), sel->transform().position(), 0.6f, sel)) {
                sel->transform().setPosition(s->position);
                sel->transform().setRotation(s->rotation); // fittings turn to match their opening
            }
        }
        // Its snap points, so you can see what will connect.
        std::function<void(GameObject&)> drawSnaps = [&](GameObject& g) {
            for (const auto& c : g.components())
                if (auto* sp = dynamic_cast<SnapPoint*>(c.get()); sp && !sp->isRemoved())
                    dd.sphere(sp->worldPosition(), view.worldSize(sp->worldPosition(), 5.0f), IM_COL32(120, 255, 160, 220));
            for (Transform* ch : g.transform().children()) drawSnaps(ch->gameObject());
        };
        drawSnaps(*sel);
        switch (ev) {
        case Gizmo::Event::Began: beginEdit(*sel); break;
        case Gizmo::Event::Ended:
            endEdit(m_gizmoOptions.mode == GizmoMode::Translate ? "Move" : m_gizmoOptions.mode == GizmoMode::Rotate ? "Rotate" : "Scale");
            afterTransformEdit(*sel);
            break;
        default: break;
        }
    }

    // --- Click to select (icons first, then GPU picking) ---
    if (ImGui::IsMouseClicked(0) && !io.WantCaptureMouse && !m_gizmo.hovered() && !m_gizmo.active()) {
        m_pressInViewport = true;
        m_pressPos = mouse;
    }
    if (ImGui::IsMouseReleased(0) && m_pressInViewport) {
        m_pressInViewport = false;
        if (glm::length(mouse - m_pressPos) < 4.0f) {
            const Icon* hit = nullptr;
            for (const Icon& ic : icons) {
                if (glm::length(ic.pos - mouse) < 11.0f) hit = &ic;
            }
            if (hit) {
                select(scene.findById(hit->id));
            } else if (m_picking) {
                const glm::vec2 px = mouse * glm::vec2(io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
                m_picking->request(glm::ivec2(px));
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Terrain tools
// -----------------------------------------------------------------------------

void Editor::drawTerrainTools(Terrain& terrain)
{
    if (!ImGui::CollapsingHeader("Terrain tools", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::PushID("terraintools");
    static const char* kModes[] = {"Off (select / move)", "Raise", "Lower", "Smooth", "Flatten", "Paint biome"};
    int mode = m_terrainTool + 1;
    if (ImGui::Combo("Brush", &mode, kModes, IM_ARRAYSIZE(kModes))) m_terrainTool = mode - 1;
    if (m_terrainTool >= 0) {
        ImGui::SliderFloat("Radius", &m_brush.radius, 1.0f, 200.0f, "%.1f m", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Strength", &m_brush.strength, 0.01f, 1.0f);
        const auto m = Terrain::Brush::Mode(m_terrainTool);
        if (m == Terrain::Brush::Mode::Flatten) {
            ImGui::DragFloat("Target height", &m_brush.height, 0.05f);
            ImGui::TextDisabled("Ctrl+click picks the height under the cursor");
        }
        if (m == Terrain::Brush::Mode::Paint && !terrain.biomes.empty()) {
            m_brush.biome = std::clamp(m_brush.biome, 0, int(terrain.biomes.size()) - 1);
            if (ImGui::BeginCombo("Biome", terrain.biomes[size_t(m_brush.biome)].name.c_str())) {
                for (size_t b = 0; b < terrain.biomes.size(); ++b)
                    if (ImGui::Selectable(terrain.biomes[b].name.c_str(), int(b) == m_brush.biome)) m_brush.biome = int(b);
                ImGui::EndCombo();
            }
        }
        ImGui::TextDisabled("Left mouse in the viewport: apply. Shift: Raise <-> Lower.");
    }
    ImGui::Text("Edited: %zu tile(s)", terrain.editedTiles());
    if (terrain.editedTiles() > 0 && ImGui::Button("Clear all edits")) {
        beginEdit(terrain);
        terrain.clearEdits();
        endEdit("Clear terrain edits");
    }
    ImGui::PopID();
}

bool Editor::terrainBrush(Terrain& terrain, const ViewProjector& view, DebugDraw& dd, glm::vec2 mouse)
{
    const ImGuiIO& io = ImGui::GetIO();
    // Ray march to the ground, then bisect.
    glm::vec3 o, d;
    view.ray(mouse, o, d);
    auto below = [&](const glm::vec3& p) { return terrain.inBounds(p.x, p.z) && p.y < terrain.height(p.x, p.z); };
    bool found = false;
    glm::vec3 hit(0.0f);
    float t0 = 0.0f;
    for (float t = 0.5f; t < 6000.0f; t += std::max(0.5f, t * 0.01f)) {
        if (below(o + d * t)) {
            float a = t0, b = t;
            for (int k = 0; k < 16; ++k) {
                const float m = 0.5f * (a + b);
                (below(o + d * m) ? b : a) = m;
            }
            hit = o + d * b;
            found = true;
            break;
        }
        t0 = t;
    }
    if (m_brushing && (!ImGui::IsMouseDown(0) || !found)) {
        m_brushing = false;
        endEdit(m_terrainTool == int(Terrain::Brush::Mode::Paint) ? "Paint terrain" : "Sculpt terrain");
    }
    if (!found) return false;

    // The brush outline, draped over the ground (outer edge and where the falloff starts).
    for (float frac : {1.0f, 0.5f}) {
        const float r = m_brush.radius * frac;
        glm::vec3 prev(0.0f);
        for (int s = 0; s <= 48; ++s) {
            const float a = float(s) / 48.0f * 6.2831853f;
            const float x = hit.x + std::cos(a) * r, z = hit.z + std::sin(a) * r;
            const glm::vec3 p(x, (terrain.inBounds(x, z) ? terrain.height(x, z) : hit.y) + 0.15f, z);
            if (s > 0) dd.line(prev, p, frac == 1.0f ? IM_COL32(255, 220, 80, 230) : IM_COL32(255, 220, 80, 110));
            prev = p;
        }
    }
    if (io.WantCaptureMouse && !m_brushing) return true;
    auto mode = Terrain::Brush::Mode(m_terrainTool);
    if (ImGui::IsMouseClicked(0)) {
        if (io.KeyCtrl && mode == Terrain::Brush::Mode::Flatten) {
            m_brush.height = hit.y;
        } else {
            beginEdit(terrain); // the whole stroke is one undo step
            m_brushing = true;
        }
    }
    if (m_brushing) {
        if (io.KeyShift && mode == Terrain::Brush::Mode::Raise) mode = Terrain::Brush::Mode::Lower;
        else if (io.KeyShift && mode == Terrain::Brush::Mode::Lower) mode = Terrain::Brush::Mode::Raise;
        Terrain::Brush b = m_brush;
        b.mode = mode;
        terrain.applyBrush(b, {hit.x, hit.z}, std::min(io.DeltaTime, 0.1f));
    }
    return true;
}

} // namespace ve
