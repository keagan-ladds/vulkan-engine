// The hierarchy panel (tree, drag & drop parenting, context menu) and the
// Create menu. See Editor.hpp.

#include "ve/editor/Editor.hpp"

#include "ve/core/Engine.hpp"
#include "ve/editor/EditorInternal.hpp"
#include "ve/physics/Physics.hpp"
#include "ve/render/Renderer.hpp"
#include "ve/scene/Prefab.hpp"
#include "ve/scene/Scene.hpp"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>

namespace ve {

using namespace editor_internal;

namespace {

glm::vec3 placementPoint(const Engine& engine, const ViewProjector& view)
{
    glm::vec3 ro, rd;
    view.ray(view.size * 0.5f, ro, rd);
    if (auto hit = const_cast<Engine&>(engine).physics().raycast(ro, rd, 300.0f)) return hit->point;
    return ro + rd * 8.0f;
}

} // namespace

void Editor::drawHierarchy()
{
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0.0f, kMenuHeight), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(kHierarchyWidth, io.DisplaySize.y - kMenuHeight), ImGuiCond_Always);
    ImGui::Begin("Hierarchy", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##filter", "Filter...", m_filter, sizeof(m_filter));
    ImGui::Checkbox("Code objects", &m_showCodeObjects);
    ImGui::Separator();

    Scene& scene = m_engine->scene();
    // Drop on empty space: unparent.
    ImGui::BeginChild("##tree");
    const std::string filter = m_filter;
    for (const auto& o : scene.objects()) {
        if (o->isDestroyed() || o->hideInHierarchy) continue;
        if (!filter.empty()) {
            std::string a = o->name(), b = filter;
            std::transform(a.begin(), a.end(), a.begin(), ::tolower);
            std::transform(b.begin(), b.end(), b.begin(), ::tolower);
            if (a.find(b) == std::string::npos) continue;
            if (!m_showCodeObjects && !o->persistent) continue;
            if (ImGui::Selectable((o->name() + "##" + std::to_string(o->id())).c_str(), o->id() == m_selected.id())) select(o.get());
            continue;
        }
        if (o->transform().parent()) continue;
        drawHierarchyNode(*o);
    }
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, std::max(ImGui::GetContentRegionAvail().y, 40.0f)));
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("VE_OBJECT")) {
            uint64_t id;
            std::memcpy(&id, p->Data, sizeof(id));
            if (GameObject* child = scene.findById(id); child && child->persistent) {
                beginEdit(*child);
                child->setParent(nullptr, true);
                endEdit("Unparent " + child->name());
            }
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::EndChild();
    ImGui::End();
}

void Editor::drawHierarchyNode(GameObject& o)
{
    if (o.isDestroyed() || o.hideInHierarchy) return;
    if (!m_showCodeObjects && !o.persistent) {
        // Still show persistent descendants of code objects.
        for (Transform* c : o.transform().children()) drawHierarchyNode(c->gameObject());
        return;
    }
    bool hasChildren = false;
    for (Transform* c : o.transform().children()) {
        if (!c->gameObject().isDestroyed() && !c->gameObject().hideInHierarchy) hasChildren = true;
    }
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (!hasChildren) flags |= ImGuiTreeNodeFlags_Leaf;
    if (o.id() == m_selected.id()) flags |= ImGuiTreeNodeFlags_Selected;

    if (!o.persistent) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.55f, 1.0f));
    else if (!o.activeSelf()) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.5f, 0.5f, 1.0f));
    ImGui::PushID(static_cast<int>(o.id() ^ (o.id() >> 32)));
    const bool open = ImGui::TreeNodeEx("##node", flags, "%s", o.name().c_str());
    if (!o.persistent || !o.activeSelf()) ImGui::PopStyleColor();
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) select(&o);

    if (ImGui::BeginDragDropSource()) {
        const uint64_t id = o.id();
        ImGui::SetDragDropPayload("VE_OBJECT", &id, sizeof(id));
        ImGui::Text("%s", o.name().c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("VE_OBJECT")) {
            uint64_t id;
            std::memcpy(&id, p->Data, sizeof(id));
            GameObject* child = m_engine->scene().findById(id);
            if (child && !child->persistent) child = nullptr; // code objects keep their hierarchy
            // No cycles: the target must not be inside the dragged object.
            bool cycle = false;
            for (Transform* t = &o.transform(); t; t = t->parent()) cycle |= child && &t->gameObject() == child;
            if (child && !cycle) {
                beginEdit(*child);
                child->setParent(&o, true);
                endEdit("Parent " + child->name());
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopupContextItem()) {
        select(&o);
        if (ImGui::MenuItem("Duplicate")) select(duplicate(o));
        if (ImGui::MenuItem("Delete", nullptr, false, o.persistent)) deleteObject(o);
        if (!o.persistent && TypeRegistry::get().of(o) && ImGui::MenuItem("Make persistent (save in scene)")) {
            beginEdit(o);
            o.persistent = true;
            const bool plain = typeid(o) == typeid(GameObject);
            for (const auto& c : o.components()) {
                if (plain && !c->isRemoved() && TypeRegistry::get().of(*c)) c->persistent = true;
            }
            endEdit("Make persistent");
        }
        ImGui::EndPopup();
    }
    if (open) {
        for (Transform* c : o.transform().children()) drawHierarchyNode(c->gameObject());
        ImGui::TreePop();
    }
    ImGui::PopID();
}

// Part of the main menu bar (drawMenuBar): new objects go in front of the camera.
void Editor::drawCreateMenu()
{
    const ViewProjector view = ViewProjector::from(m_engine->renderer().frameData().camera,
                                                   {ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y});
    auto newObject = [&](const std::string& type, const std::string& name, Json components = Json::array()) {
        const glm::vec3 p = placementPoint(*m_engine, view);
        Json j{{"type", type}, {"name", name}, {"position", {p.x, p.y, p.z}}};
        if (!components.empty()) j["components"] = std::move(components);
        createObject(j, nullptr, "Create " + name);
    };

    if (ImGui::BeginMenu("Create")) {
        if (ImGui::MenuItem("Empty")) newObject("GameObject", "GameObject");
        if (ImGui::BeginMenu("Primitive")) {
            const char* shapes[] = {"Cube", "Sphere", "Cylinder", "Plane"};
            for (int i = 0; i < 4; ++i) {
                if (ImGui::MenuItem(shapes[i])) {
                    Json size = i == 3 ? Json{10.0f, 1.0f, 10.0f} : Json{1.0f, 1.0f, 1.0f};
                    newObject("GameObject", shapes[i],
                              Json::array({{{"type", "PrimitiveRenderer"}, {"fields", {{"shape", i}, {"size", size}}}}}));
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Model")) {
            const auto models = listAssets(".gltf .glb");
            if (models.empty()) ImGui::TextDisabled("No .gltf/.glb files in assets/");
            for (const auto& m : models) {
                if (ImGui::MenuItem(m.c_str())) {
                    const std::string name = std::filesystem::path(m).stem().string();
                    newObject("GameObject", name,
                              Json::array({{{"type", "ModelInstance"}, {"fields", {{"path", m}, {"staticColliders", true}}}}}));
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Prefab")) {
            const auto prefabs = listAssets(".prefab");
            if (prefabs.empty()) ImGui::TextDisabled("No .prefab files in assets/ (Edit > Save as prefab)");
            for (const auto& path : prefabs) {
                if (!ImGui::MenuItem(path.c_str())) continue;
                if (const Json* pj = prefab::load(path)) {
                    // A copy of the prefab's object, linked back to it, in front of the camera.
                    const auto obj = pj->find("object");
                    Json j = obj != pj->end() ? *obj : Json::object(); // (copied once: it's edited below)
                    const glm::vec3 p = placementPoint(*m_engine, view);
                    j["position"] = {p.x, p.y, p.z};
                    Json& comps = j["components"];
                    if (!comps.is_array()) comps = Json::array();
                    comps.push_back({{"type", "PrefabInstance"}, {"fields", {{"prefab", path}}}});
                    createObject(j, nullptr, "Create " + std::filesystem::path(path).stem().string());
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Light")) {
            if (ImGui::MenuItem("Point light")) newObject("GameObject", "Point Light", Json::array({{{"type", "PointLight"}}}));
            if (ImGui::MenuItem("Spot light")) newObject("GameObject", "Spot Light", Json::array({{{"type", "SpotLight"}}}));
            if (ImGui::MenuItem("Directional light"))
                newObject("GameObject", "Directional Light", Json::array({{{"type", "DirectionalLight"}}}));
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Decal")) {
            // A 2 x 2 m box projecting straight down onto the ground (rotated -90 degrees about X).
            const glm::vec3 p = placementPoint(*m_engine, view);
            const float h = 0.70710678f;
            Json j{{"type", "GameObject"}, {"name", "Decal"}, {"position", {p.x, p.y, p.z}},
                   {"rotation", {-h, 0.0f, 0.0f, h}}, {"scale", {2.0f, 2.0f, 1.0f}},
                   {"components", Json::array({{{"type", "DecalProjector"}}})}};
            createObject(j, nullptr, "Create Decal");
        }
        if (ImGui::MenuItem("Spline")) {
            // A short straight spline on the ground in front of the camera, ready to edit.
            const glm::vec3 p = placementPoint(*m_engine, view);
            Json j{{"type", "GameObject"}, {"name", "Spline"}, {"position", {p.x, p.y, p.z}},
                   {"components", Json::array({{{"type", "Spline"},
                                                {"fields", {{"points", Json::array({{-5.0f, 0.0f, 0.0f, 1.0f}, {5.0f, 0.0f, 0.0f, 1.0f}})}}}}})}};
            if (GameObject* o = createObject(j, nullptr, "Create Spline")) {
                select(o);
                m_splineEdit = true; // straight into point editing (Ctrl+click adds points)
                m_splinePoint = 1;
            }
        }
        if (ImGui::BeginMenu("Audio")) {
            if (ImGui::MenuItem("Audio source")) newObject("GameObject", "Audio Source", Json::array({{{"type", "AudioSource"}}}));
            if (ImGui::MenuItem("Audio zone"))
                newObject("GameObject", "Audio Zone",
                          Json::array({{{"type", "AudioZone"},
                                        {"fields", {{"effects", Json::array({{{"bus", "Weather"}, {"volume", 0.3f}, {"lowpass", 450.0f}}})}}}}}));
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Camera")) newObject("GameObject", "Camera", Json::array({{{"type", "Camera"}}}));
        // Game-registered GameObject classes, by category.
        std::string category;
        bool open = false;
        for (const TypeInfo* t : TypeRegistry::get().all(false)) {
            if (!t->creatable || t->name == "GameObject") continue;
            if (t->category != category) {
                if (open) ImGui::EndMenu();
                category = t->category;
                open = ImGui::BeginMenu(category.c_str());
            }
            if (open && ImGui::MenuItem(t->name.c_str())) newObject(t->name, t->name);
        }
        if (open) ImGui::EndMenu();
        ImGui::EndMenu();
    }
}

} // namespace ve
