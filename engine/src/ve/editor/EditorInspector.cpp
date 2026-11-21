// The inspector panel: the selection's name / transform, its class and
// component fields (drawField), adding and removing components. See Editor.hpp.

#include "ve/editor/Editor.hpp"

#include "ve/editor/EditorInternal.hpp"
#include "ve/scene/Components.hpp"
#include "ve/scene/Scene.hpp"
#include "ve/scene/SceneSerializer.hpp"

#include <glm/gtc/quaternion.hpp>
#include <imgui.h>

#include <cctype>
#include <cstdio>
#include <typeinfo>

namespace ve {

using namespace editor_internal;

namespace {

std::string componentLabel(const Component& c)
{
    if (const TypeInfo* t = TypeRegistry::get().of(c)) return t->name;
    std::string n = typeid(c).name();
    // Strip Itanium mangling noise roughly ("N2ve10AudioZoneE" -> "AudioZone").
    std::string out;
    for (size_t i = 0; i < n.size();) {
        if (std::isdigit(static_cast<unsigned char>(n[i]))) {
            size_t len = 0;
            while (i < n.size() && std::isdigit(static_cast<unsigned char>(n[i]))) len = len * 10 + size_t(n[i++] - '0');
            out = n.substr(i, len);
            i += len;
        } else {
            ++i;
        }
    }
    return out.empty() ? n : out;
}

} // namespace

bool Editor::drawField(const FieldInfo& f, void* obj, GameObject& owner)
{
    if (f.hidden) return false;
    bool changed = false;
    const bool ranged = f.max > f.min;
    const char* label = f.name.c_str();
    if (f.readOnly) ImGui::BeginDisabled();
    switch (f.kind) {
    case FieldKind::Bool: {
        bool v = f.get(obj).get<bool>();
        if (ImGui::Checkbox(label, &v)) { f.set(obj, v); changed = true; }
        break;
    }
    case FieldKind::Int: {
        int v = f.get(obj).get<int>();
        if (ImGui::DragInt(label, &v, f.speed, ranged ? int(f.min) : 0, ranged ? int(f.max) : 0)) { f.set(obj, v); changed = true; }
        break;
    }
    case FieldKind::Float: {
        float v = f.get(obj).get<float>();
        if (ImGui::DragFloat(label, &v, f.speed, f.min, f.max, "%.3f", ranged ? ImGuiSliderFlags_AlwaysClamp : 0)) {
            f.set(obj, v);
            changed = true;
        }
        break;
    }
    case FieldKind::Vec2:
    case FieldKind::Vec3:
    case FieldKind::Vec4:
    case FieldKind::Color3:
    case FieldKind::Color4: {
        Json j = f.get(obj);
        float v[4] = {0, 0, 0, 0};
        for (size_t i = 0; i < j.size() && i < 4; ++i) v[i] = j[i].get<float>();
        bool c = false;
        switch (f.kind) {
        case FieldKind::Vec2: c = ImGui::DragFloat2(label, v, f.speed, f.min, f.max); break;
        case FieldKind::Vec3: c = ImGui::DragFloat3(label, v, f.speed, f.min, f.max); break;
        case FieldKind::Vec4: c = ImGui::DragFloat4(label, v, f.speed, f.min, f.max); break;
        case FieldKind::Color3: c = ImGui::ColorEdit3(label, v, ImGuiColorEditFlags_Float); break;
        default: c = ImGui::ColorEdit4(label, v, ImGuiColorEditFlags_Float); break;
        }
        if (c) {
            Json out = Json::array();
            for (size_t i = 0; i < j.size(); ++i) out.push_back(v[i]);
            f.set(obj, out);
            changed = true;
        }
        break;
    }
    case FieldKind::String: {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s", f.get(obj).get<std::string>().c_str());
        if (ImGui::InputText(label, buf, sizeof(buf))) { f.set(obj, std::string(buf)); changed = true; }
        break;
    }
    case FieldKind::Enum: {
        int v = f.get(obj).get<int>();
        const char* preview = v >= 0 && v < int(f.enumNames.size()) ? f.enumNames[size_t(v)].c_str() : "?";
        if (ImGui::BeginCombo(label, preview)) {
            for (int i = 0; i < int(f.enumNames.size()); ++i) {
                if (ImGui::Selectable(f.enumNames[size_t(i)].c_str(), i == v)) { f.set(obj, i); changed = true; }
            }
            ImGui::EndCombo();
        }
        break;
    }
    case FieldKind::Asset: {
        const std::string cur = f.get(obj).get<std::string>();
        if (ImGui::BeginCombo(label, cur.empty() ? "(none)" : cur.c_str())) {
            if (ImGui::Selectable("(none)", cur.empty())) { f.set(obj, std::string()); changed = true; }
            for (const auto& a : listAssets(f.assetFilter)) {
                if (ImGui::Selectable(a.c_str(), a == cur)) { f.set(obj, a); changed = true; }
            }
            ImGui::EndCombo();
        }
        break;
    }
    case FieldKind::Json: {
        std::string text = f.get(obj).dump(1);
        char buf[2048];
        std::snprintf(buf, sizeof(buf), "%s", text.c_str());
        ImGui::TextUnformatted(label);
        if (ImGui::InputTextMultiline((std::string("##") + f.name).c_str(), buf, sizeof(buf), ImVec2(-1.0f, 90.0f),
                                      ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine)) {
            try {
                f.set(obj, Json::parse(buf));
                changed = true;
            } catch (...) {
                m_status = "Invalid JSON for " + f.name;
                m_statusTime = 3.0f;
            }
        }
        break;
    }
    }
    if (f.readOnly) ImGui::EndDisabled();
    if (!f.tooltip.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", f.tooltip.c_str());

    // Undo bookkeeping: drags span many frames (activate .. deactivate); clicks
    // (checkbox, combo) change and finish in one frame.
    if (ImGui::IsItemActivated()) beginEdit(owner);
    if (changed && !m_editing) {
        m_editing = true; // m_editBefore was captured at the start of the inspector frame
    }
    if (ImGui::IsItemDeactivatedAfterEdit() || (changed && !ImGui::IsItemActive())) endEdit("Edit " + f.name);
    else if (ImGui::IsItemDeactivated() && m_editing && !changed) m_editing = false;
    return changed;
}

void Editor::drawInspector()
{
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - kInspectorWidth, kMenuHeight), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(kInspectorWidth, io.DisplaySize.y - kMenuHeight), ImGuiCond_Always);
    ImGui::Begin("Inspector", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    GameObject* sel = selected();
    if (!sel) {
        ImGui::TextDisabled("Nothing selected.");
        ImGui::TextDisabled("Click an object in the view or the hierarchy.");
        ImGui::End();
        return;
    }
    // "Before" state for one-frame edits (checkbox / combo), see drawField.
    refreshEditBefore(*sel, io.DeltaTime);
    ImGui::PushItemWidth(-130.0f);

    // --- Object ---
    char name[128];
    std::snprintf(name, sizeof(name), "%s", sel->name().c_str());
    if (ImGui::InputText("Name", name, sizeof(name))) sel->setName(name);
    if (ImGui::IsItemActivated()) beginEdit(*sel);
    if (ImGui::IsItemDeactivated()) endEdit("Rename");
    bool active = sel->activeSelf();
    if (ImGui::Checkbox("Active", &active)) {
        m_editing = true;
        sel->setActive(active);
        endEdit(active ? "Activate" : "Deactivate");
    }
    ImGui::SameLine();
    const TypeInfo* type = TypeRegistry::get().of(*sel);
    if (sel->persistent) {
        ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "saved in scene");
    } else {
        ImGui::TextDisabled("code object (not saved)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Created by game code. Edits are live but not saved.\nRight-click it in the hierarchy to make it persistent.");
    }
    ImGui::TextDisabled("Type: %s   id %s", type ? type->name.c_str() : "(unregistered)", SceneSerializer::idToString(sel->id()).c_str());

    // --- Transform (local) ---
    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        Transform& t = sel->transform();
        bool moved = false;
        auto track = [&](bool changed, const char* label) {
            if (ImGui::IsItemActivated()) beginEdit(*sel);
            if (changed) moved = true;
            if (ImGui::IsItemDeactivated()) {
                endEdit(label);
                afterTransformEdit(*sel);
            }
        };
        glm::vec3 p = t.localPosition();
        track(ImGui::DragFloat3("Position", &p.x, 0.05f) && (t.setLocalPosition(p), true), "Move");
        // Euler angles are cached per object so dragging doesn't flip at gimbal angles.
        if (m_eulerObject.id() != sel->id() || glm::abs(glm::dot(m_eulerFor, t.localRotation())) < 0.99999f) {
            m_eulerCache = glm::degrees(glm::eulerAngles(t.localRotation()));
            m_eulerFor = t.localRotation();
            m_eulerObject = sel;
        }
        glm::vec3 e = m_eulerCache;
        if (ImGui::DragFloat3("Rotation", &e.x, 0.5f)) {
            m_eulerCache = e;
            t.setLocalRotation(glm::quat(glm::radians(e)));
            m_eulerFor = t.localRotation();
            moved = true;
        }
        track(false, "Rotate");
        glm::vec3 s = t.localScale();
        track(ImGui::DragFloat3("Scale", &s.x, 0.01f, 0.001f, 1000.0f) && (t.setLocalScale(s), true), "Scale");
        (void)moved;
    }

    // --- Class fields ---
    if (type && !type->fields.empty() && ImGui::CollapsingHeader(type->name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("class");
        bool changed = false;
        for (const FieldInfo& f : type->fields) changed |= drawField(f, sel, *sel);
        if (changed && type->onChanged) type->onChanged(sel);
        ImGui::PopID();
    }
    if (auto* terrain = dynamic_cast<Terrain*>(sel)) drawTerrainTools(*terrain);

    // --- Components ---
    Component* removeMe = nullptr;
    int index = 0;
    // A snapshot: editing a field can rebuild the object, which may remove
    // sibling components (PrimitiveRenderer's MeshRenderer, Terrain colliders).
    m_inspectScratch.clear();
    for (const auto& cp : sel->components()) m_inspectScratch.push_back(cp.get());
    for (Component* cp : m_inspectScratch) {
        if (cp->isRemoved()) continue; // (removed this frame; still alive)
        Component& c = *cp;
        const TypeInfo* ct = TypeRegistry::get().of(c);
        ImGui::PushID(index++);
        std::string header = componentLabel(c) + (c.persistent ? "" : "  (code)");
        const bool open = ImGui::CollapsingHeader((header + "###comp").c_str(), ImGuiTreeNodeFlags_DefaultOpen);
        if (c.persistent && ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Remove component")) removeMe = &c;
            ImGui::EndPopup();
        }
        if (open) {
            bool en = c.enabled;
            if (ImGui::Checkbox("Enabled", &en)) {
                m_editing = true;
                c.enabled = en;
                // (A static renderer is batched on the GPU: it must hear about it.)
                if (auto* mr = dynamic_cast<MeshRenderer*>(&c)) mr->refresh();
                endEdit("Toggle " + componentLabel(c));
            }
            if (c.persistent) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove")) removeMe = &c;
            }
            if (ct) {
                bool changed = false;
                for (const FieldInfo& f : ct->fields) changed |= drawField(f, &c, *sel);
                if (changed && ct->onChanged) ct->onChanged(&c);
            } else {
                ImGui::TextDisabled("Not registered: no editable fields.");
            }
        }
        ImGui::PopID();
    }
    if (removeMe) {
        beginEdit(*sel);
        sel->removeComponent(removeMe);
        endEdit("Remove component");
    }

    // --- Add component ---
    ImGui::Separator();
    if (!sel->persistent) ImGui::BeginDisabled();
    if (ImGui::Button("Add component", ImVec2(-1.0f, 0.0f))) ImGui::OpenPopup("addcomp");
    if (!sel->persistent) {
        ImGui::EndDisabled();
        ImGui::TextDisabled("(make the object persistent to add components)");
    }
    if (ImGui::BeginPopup("addcomp")) {
        std::string category;
        for (const TypeInfo* t : TypeRegistry::get().all(true)) {
            if (!t->creatable || !t->addTo) continue;
            if (t->category != category) {
                category = t->category;
                ImGui::SeparatorText(category.c_str());
            }
            if (ImGui::MenuItem(t->name.c_str())) {
                beginEdit(*sel);
                Component* c = t->addTo(*sel);
                c->persistent = true;
                endEdit("Add " + t->name);
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopItemWidth();
    ImGui::End();
}

} // namespace ve
