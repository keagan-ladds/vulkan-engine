#include "ve/editor/Editor.hpp"

#include "ve/assets/AssetManager.hpp"
#include "ve/audio/Audio.hpp"
#include "ve/core/Engine.hpp"
#include "ve/core/Input.hpp"
#include "ve/core/Log.hpp"
#include "ve/editor/PickingPass.hpp"
#include "ve/physics/Physics.hpp"
#include "ve/render/Mesh.hpp"
#include "ve/render/Renderer.hpp"
#include "ve/scene/Components.hpp"
#include "ve/scene/MeshComponents.hpp"
#include "ve/scene/Scene.hpp"
#include "ve/scene/SceneSerializer.hpp"
#include "ve/ui/UiLayer.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <set>
#include <sstream>

namespace ve {

namespace {

constexpr float kMenuHeight = 22.0f;
constexpr float kHierarchyWidth = 290.0f;
constexpr float kInspectorWidth = 370.0f;

GameObject* persistentAncestorOrRoot(GameObject* o)
{
    GameObject* root = o;
    for (GameObject* g = o; g; g = g->transform().parent() ? &g->transform().parent()->gameObject() : nullptr) {
        if (g->persistent) return g;
        root = g;
    }
    return root;
}

template <class F>
void forSubtree(GameObject& o, F&& fn)
{
    fn(o);
    for (Transform* c : o.transform().children()) forSubtree(c->gameObject(), fn);
}

bool subtreeBounds(GameObject& o, AABB& out)
{
    bool any = false;
    forSubtree(o, [&](GameObject& g) {
        if (g.isDestroyed()) return;
        if (auto* mr = g.getComponent<MeshRenderer>(); mr && mr->mesh) {
            const AABB b = mr->mesh->bounds().transformed(g.transform().worldMatrix());
            if (b.valid()) {
                out.expand(b.min);
                out.expand(b.max);
                any = true;
            }
        }
    });
    return any;
}

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

glm::vec3 placementPoint(const Engine& engine, const ViewProjector& view)
{
    glm::vec3 ro, rd;
    view.ray(view.size * 0.5f, ro, rd);
    if (auto hit = const_cast<Engine&>(engine).physics().raycast(ro, rd, 300.0f)) return hit->point;
    return ro + rd * 8.0f;
}

} // namespace

// -----------------------------------------------------------------------------
// Setup / modes
// -----------------------------------------------------------------------------

void Editor::init(Engine& engine)
{
    m_engine = &engine;
    m_picking = &engine.renderer().graph().addPass<PickingPass>();
}

void Editor::shutdown()
{
    m_camera = nullptr;
    m_picking = nullptr;
}

void Editor::setMode(Mode mode)
{
    // Before the game has run a couple of frames its camera isn't placed yet:
    // apply the switch once it is (e.g. a game that starts in the editor).
    if (m_frames < 3) {
        m_pendingMode = mode;
        m_hasPending = true;
        return;
    }
    if (mode == m_mode) return;
    Scene& scene = m_engine->scene();
    Input& in = m_engine->input();

    if (m_mode == Mode::Play && mode != Mode::Play && !m_playSnapshot.is_null()) {
        // Stop: every persistent object back to how it was when Play was pressed.
        SceneSerializer::clearPersistent(scene);
        SceneSerializer::loadJson(scene, m_playSnapshot);
        m_playSnapshot = Json();
        m_status = "Stopped: scene restored";
        m_statusTime = 3.0f;
    }
    if (mode == Mode::Play) m_playSnapshot = SceneSerializer::serializeScene(scene);

    if (mode == Mode::Edit) {
        in.setMouseCaptured(false);
        // The game's camera (highest priority active Camera that isn't ours).
        Camera* game = nullptr;
        scene.forEach<Camera>([&](Camera& c) {
            if (c.isActive() && &c.gameObject() != m_camera && (!game || c.priority > game->priority)) game = &c;
        });
        if (!m_camera) {
            m_camera = &scene.spawn("Editor Camera");
            m_camera->hideInHierarchy = true;
            m_camera->runInEditor = true;
            auto& cam = m_camera->addComponent<Camera>();
            cam.priority = 1000000;
            cam.farPlane = 6000.0f;
        }
        if (m_mode == Mode::Off && game) {
            // Start where the game camera is.
            const glm::vec3 f = game->transform().forward();
            m_camera->transform().setPosition(game->transform().position());
            m_camYaw = std::atan2(-f.x, -f.z);
            m_camPitch = std::asin(glm::clamp(f.y, -1.0f, 1.0f));
        }
        m_camera->setActive(true);
    } else if (m_camera) {
        m_camera->setActive(false);
        m_looking = false;
    }
    m_mode = mode;
}

GameObject* Editor::selected() const
{
    if (!m_selected) return nullptr;
    GameObject* o = m_engine->scene().findById(m_selected);
    return o && !o->isDestroyed() ? o : nullptr;
}

void Editor::select(GameObject* object)
{
    m_selected = object ? object->id() : 0;
}

void Editor::requestPick(glm::ivec2 pixel)
{
    if (m_picking) m_picking->request(pixel);
}

// -----------------------------------------------------------------------------
// Frame
// -----------------------------------------------------------------------------

void Editor::beginFrame(float dt)
{
    Input& in = m_engine->input();
    if (m_frames < 3 && ++m_frames == 3 && m_hasPending) {
        m_hasPending = false;
        setMode(m_pendingMode);
    }
    if (in.keyPressed(Key::F2)) setMode(m_mode == Mode::Off ? Mode::Edit : m_mode == Mode::Edit ? Mode::Off : Mode::Edit);
    m_statusTime = std::max(0.0f, m_statusTime - dt);
    m_assetCacheAge += dt;
    if (m_mode != Mode::Edit) return;

    updateCamera(dt);
    if (m_picking) {
        if (auto id = m_picking->result()) {
            GameObject* o = *id ? m_engine->scene().findById(*id) : nullptr;
            select(o ? persistentAncestorOrRoot(o) : nullptr);
        }
    }
}

void Editor::updateCamera(float dt)
{
    if (!m_camera) return;
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
    Transform& t = m_camera->transform();
    t.setRotation(glm::quat(glm::vec3(m_camPitch, m_camYaw, 0.0f)));
    if (m_looking) {
        glm::vec3 move = t.forward() * in.axis(Key::S, Key::W) + t.right() * in.axis(Key::A, Key::D);
        move.y += in.axis(Key::Q, Key::E);
        t.translate(move * m_camSpeed * (in.keyDown(Key::LShift) ? 4.0f : 1.0f) * dt);
    } else if (!io.WantCaptureMouse && in.wheel() != 0.0f) {
        t.translate(t.forward() * in.wheel() * m_camSpeed * 0.25f); // dolly
    }
}

void Editor::handleShortcuts()
{
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || m_looking) return;
    const bool ctrl = io.KeyCtrl;
    GameObject* sel = selected();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z)) io.KeyShift ? redo() : undo();
    else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y)) redo();
    else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S)) {
        if (m_scenePath.empty()) m_openSaveAs = true;
        else saveScene(m_scenePath);
    } else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_D) && sel) {
        select(duplicate(*sel));
    } else if (!ctrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_W)) m_gizmoOptions.mode = GizmoMode::Translate;
        if (ImGui::IsKeyPressed(ImGuiKey_E)) m_gizmoOptions.mode = GizmoMode::Rotate;
        if (ImGui::IsKeyPressed(ImGuiKey_R)) m_gizmoOptions.mode = GizmoMode::Scale;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) select(nullptr);
        if (ImGui::IsKeyPressed(ImGuiKey_Delete) && sel) deleteObject(*sel);
        if (ImGui::IsKeyPressed(ImGuiKey_F) && sel && m_camera) {
            AABB b;
            glm::vec3 c = sel->transform().position();
            float r = 2.0f;
            if (subtreeBounds(*sel, b)) {
                c = b.center();
                r = std::max(glm::length(b.extents()), 0.5f);
            }
            m_camera->transform().setPosition(c - m_camera->transform().forward() * (r * 2.5f + 1.0f));
        }
    }
}

void Editor::onGui()
{
    if (m_mode == Mode::Off) return;
    if (m_mode == Mode::Play) {
        drawPlayToolbar();
        return;
    }
    handleShortcuts();
    drawMenuBar();
    drawHierarchy();
    drawInspector();
    drawViewport();

    if (m_openSaveAs) {
        ImGui::OpenPopup("Save scene as");
        m_openSaveAs = false;
    }
    if (ImGui::BeginPopupModal("Save scene as", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Path (relative to the assets folder):");
        ImGui::SetNextItemWidth(360.0f);
        const bool enter = ImGui::InputText("##path", m_saveAsBuffer, sizeof(m_saveAsBuffer), ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::Button("Save") || enter) {
            saveScene(m_saveAsBuffer);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// -----------------------------------------------------------------------------
// Undo / redo
// -----------------------------------------------------------------------------

Editor::ObjectState Editor::snapshot(uint64_t id) const
{
    ObjectState s;
    s.id = id;
    if (GameObject* o = m_engine->scene().findById(id); o && !o->isDestroyed()) {
        s.json = SceneSerializer::serializeObject(*o);
        if (const Transform* p = o->transform().parent()) s.parent = p->gameObject().id();
    }
    return s;
}

void Editor::applyStates(const std::vector<ObjectState>& states)
{
    Scene& scene = m_engine->scene();
    for (const ObjectState& st : states) {
        GameObject* o = scene.findById(st.id);
        if (o && o->isDestroyed()) o = nullptr;
        if (st.json.is_null()) {
            if (o) o->destroy();
            continue;
        }
        GameObject* parent = st.parent ? scene.findById(st.parent) : nullptr;
        if (o && SceneSerializer::applyObject(*o, st.json)) {
            const Transform* want = parent ? &parent->transform() : nullptr;
            if (o->transform().parent() != want) o->setParent(parent, false);
            afterTransformEdit(*o);
        } else {
            if (o) o->destroy();
            SceneSerializer::createObject(scene, st.json, parent, false);
        }
    }
}

void Editor::pushUndo(UndoEntry entry)
{
    m_undo.push_back(std::move(entry));
    if (m_undo.size() > 200) m_undo.erase(m_undo.begin());
    m_redo.clear();
}

void Editor::undo()
{
    if (m_undo.empty()) return;
    UndoEntry e = std::move(m_undo.back());
    m_undo.pop_back();
    applyStates(e.before);
    m_status = "Undo: " + e.label;
    m_statusTime = 2.0f;
    m_redo.push_back(std::move(e));
}

void Editor::redo()
{
    if (m_redo.empty()) return;
    UndoEntry e = std::move(m_redo.back());
    m_redo.pop_back();
    applyStates(e.after);
    m_status = "Redo: " + e.label;
    m_statusTime = 2.0f;
    m_undo.push_back(std::move(e));
}

void Editor::beginEdit(GameObject& object)
{
    if (m_editing) return;
    m_editing = true;
    m_editBefore = snapshot(object.id());
}

void Editor::endEdit(const std::string& label)
{
    if (!m_editing) return;
    m_editing = false;
    ObjectState after = snapshot(m_editBefore.id);
    if (after.json == m_editBefore.json && after.parent == m_editBefore.parent) return;
    pushUndo({label, {m_editBefore}, {after}});
}

void Editor::afterTransformEdit(GameObject& object)
{
    // Physics bodies don't follow Transform edits by themselves (static ones never move).
    forSubtree(object, [](GameObject& g) {
        for (const auto& c : g.components()) {
            if (auto* rb = dynamic_cast<Rigidbody*>(c.get())) rb->rebuild();
        }
    });
}

GameObject* Editor::createObject(const Json& json, GameObject* parent, const std::string& label)
{
    GameObject* o = SceneSerializer::createObject(m_engine->scene(), json, parent, true);
    ObjectState before;
    before.id = o->id();
    before.parent = parent ? parent->id() : 0;
    pushUndo({label, {before}, {snapshot(o->id())}});
    select(o);
    return o;
}

void Editor::deleteObject(GameObject& object)
{
    if (&object == m_camera) return;
    if (!object.persistent) {
        // Game code may hold pointers to its own objects (and worker threads may
        // use them): deleting those could crash the game.
        m_status = "Can't delete '" + object.name() + "': created by game code";
        m_statusTime = 3.0f;
        return;
    }
    ObjectState before = snapshot(object.id());
    ObjectState after;
    after.id = before.id;
    after.parent = before.parent;
    object.destroy();
    pushUndo({"Delete " + object.name(), {before}, {after}});
    if (m_selected == object.id()) select(nullptr);
}

GameObject* Editor::duplicate(GameObject& object)
{
    GameObject* parent = object.transform().parent() ? &object.transform().parent()->gameObject() : nullptr;
    Json j = SceneSerializer::serializeObject(object);
    GameObject* copy = createObject(j, parent, "Duplicate " + object.name());
    if (!object.persistent) {
        // Duplicating a code object gives a persistent copy of what the type can describe.
        copy->persistent = true;
    }
    return copy;
}

// -----------------------------------------------------------------------------
// Scene files
// -----------------------------------------------------------------------------

bool Editor::saveScene(const std::filesystem::path& path)
{
    const bool ok = SceneSerializer::save(m_engine->scene(), path);
    if (ok) m_scenePath = path.generic_string();
    m_status = ok ? "Saved " + path.generic_string() : "Save failed: " + path.generic_string();
    m_statusTime = 3.0f;
    return ok;
}

void Editor::openScene(const std::filesystem::path& path)
{
    SceneSerializer::clearPersistent(m_engine->scene());
    SceneSerializer::load(m_engine->scene(), path);
    m_scenePath = path.generic_string();
    m_undo.clear();
    m_redo.clear();
    select(nullptr);
    m_status = "Opened " + m_scenePath;
    m_statusTime = 3.0f;
}

void Editor::newScene()
{
    SceneSerializer::clearPersistent(m_engine->scene());
    m_scenePath.clear();
    m_undo.clear();
    m_redo.clear();
    select(nullptr);
}

std::vector<std::string> Editor::listAssets(const std::string& extensions)
{
    if (extensions == m_assetCacheKey && m_assetCacheAge < 3.0f) return m_assetCache;
    std::set<std::string> found;
    std::istringstream exts(extensions);
    std::vector<std::string> wanted;
    for (std::string e; exts >> e;) wanted.push_back(e);
    for (const auto& root : m_engine->assets().searchPaths()) {
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec)) continue;
        for (auto it = std::filesystem::recursive_directory_iterator(root, ec); it != std::filesystem::recursive_directory_iterator();
             it.increment(ec)) {
            if (ec || !it->is_regular_file()) continue;
            std::string ext = it->path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (std::find(wanted.begin(), wanted.end(), ext) == wanted.end()) continue;
            found.insert(std::filesystem::relative(it->path(), root, ec).generic_string());
        }
    }
    m_assetCache.assign(found.begin(), found.end());
    m_assetCacheKey = extensions;
    m_assetCacheAge = 0.0f;
    return m_assetCache;
}

// -----------------------------------------------------------------------------
// Panels
// -----------------------------------------------------------------------------

void Editor::drawPlayToolbar()
{
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 6.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("##play", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove);
    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Playing");
    ImGui::SameLine();
    if (ImGui::Button("Stop (F2)")) setMode(Mode::Edit);
    ImGui::End();
}

void Editor::drawMenuBar()
{
    Scene& scene = m_engine->scene();
    const ViewProjector view = ViewProjector::from(m_engine->renderer().frameData().camera,
                                                   {ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y});
    auto newObject = [&](const std::string& type, const std::string& name, Json components = Json::array()) {
        const glm::vec3 p = placementPoint(*m_engine, view);
        Json j{{"type", type}, {"name", name}, {"position", {p.x, p.y, p.z}}};
        if (!components.empty()) j["components"] = std::move(components);
        createObject(j, nullptr, "Create " + name);
    };

    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New scene")) newScene();
        if (ImGui::BeginMenu("Open")) {
            bool any = false;
            for (const auto& f : listAssets(".json")) {
                if (f.rfind("scenes/", 0) != 0) continue;
                any = true;
                if (ImGui::MenuItem(f.c_str())) openScene(f);
            }
            if (!any) ImGui::TextDisabled("No scenes in assets/scenes/");
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Save", "Ctrl+S")) {
            if (m_scenePath.empty()) m_openSaveAs = true;
            else saveScene(m_scenePath);
        }
        if (ImGui::MenuItem("Save as...")) {
            std::snprintf(m_saveAsBuffer, sizeof(m_saveAsBuffer), "%s",
                          m_scenePath.empty() ? "scenes/untitled.json" : m_scenePath.c_str());
            m_openSaveAs = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit editor", "F2")) setMode(Mode::Off);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        const std::string u = m_undo.empty() ? "Undo" : "Undo " + m_undo.back().label;
        const std::string r = m_redo.empty() ? "Redo" : "Redo " + m_redo.back().label;
        if (ImGui::MenuItem(u.c_str(), "Ctrl+Z", false, !m_undo.empty())) undo();
        if (ImGui::MenuItem(r.c_str(), "Ctrl+Y", false, !m_redo.empty())) redo();
        GameObject* sel = selected();
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, sel)) select(duplicate(*sel));
        if (ImGui::MenuItem("Delete", "Del", false, sel)) deleteObject(*sel);
        ImGui::EndMenu();
    }
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
        if (ImGui::BeginMenu("Light")) {
            if (ImGui::MenuItem("Point light")) newObject("GameObject", "Point Light", Json::array({{{"type", "PointLight"}}}));
            if (ImGui::MenuItem("Directional light"))
                newObject("GameObject", "Directional Light", Json::array({{{"type", "DirectionalLight"}}}));
            ImGui::EndMenu();
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
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Show code objects", nullptr, &m_showCodeObjects);
        ImGui::MenuItem("Show icons", nullptr, &m_showIcons);
        ImGui::MenuItem("Engine overlay (F1)", nullptr, &m_engine->ui().debugOverlay);
        ImGui::EndMenu();
    }

    // Toolbar.
    ImGui::Separator();
    if (ImGui::Button("Play")) setMode(Mode::Play);
    ImGui::Separator();
    auto modeButton = [&](const char* label, GizmoMode mode) {
        const bool on = m_gizmoOptions.mode == mode;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(label)) m_gizmoOptions.mode = mode;
        if (on) ImGui::PopStyleColor();
    };
    modeButton("Move (W)", GizmoMode::Translate);
    modeButton("Rotate (E)", GizmoMode::Rotate);
    modeButton("Scale (R)", GizmoMode::Scale);
    if (ImGui::Button(m_gizmoOptions.local ? "Local" : "World")) m_gizmoOptions.local = !m_gizmoOptions.local;
    ImGui::Checkbox("Snap", &m_gizmoOptions.snap);
    if (m_gizmoOptions.snap) {
        ImGui::SetNextItemWidth(60.0f);
        ImGui::DragFloat("##snapm", &m_gizmoOptions.snapMove, 0.05f, 0.01f, 100.0f, "%.2f m");
        ImGui::SetNextItemWidth(55.0f);
        ImGui::DragFloat("##snapa", &m_gizmoOptions.snapAngle, 0.5f, 1.0f, 90.0f, "%.0f deg");
    }
    ImGui::Separator();
    ImGui::TextDisabled("%s", m_scenePath.empty() ? "(unsaved scene)" : m_scenePath.c_str());
    if (m_statusTime > 0.0f) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "  %s", m_status.c_str());
    }
    ImGui::SameLine(ImGui::GetWindowWidth() - 330.0f);
    ImGui::TextDisabled("RMB+WASD fly  F focus  F2 exit");
    ImGui::EndMainMenuBar();
    (void)scene;
}

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
            if (ImGui::Selectable((o->name() + "##" + std::to_string(o->id())).c_str(), o->id() == m_selected)) select(o.get());
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
    if (o.id() == m_selected) flags |= ImGuiTreeNodeFlags_Selected;

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
                if (plain && TypeRegistry::get().of(*c)) c->persistent = true;
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

bool Editor::drawField(const FieldInfo& f, void* obj, GameObject& owner)
{
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
    if (!m_editing) m_editBefore = snapshot(sel->id());
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
        if (m_eulerObject != sel->id() || glm::abs(glm::dot(m_eulerFor, t.localRotation())) < 0.99999f) {
            m_eulerCache = glm::degrees(glm::eulerAngles(t.localRotation()));
            m_eulerFor = t.localRotation();
            m_eulerObject = sel->id();
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

    // --- Components ---
    Component* removeMe = nullptr;
    int index = 0;
    for (const auto& cp : sel->components()) {
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

void Editor::drawViewport()
{
    const ImGuiIO& io = ImGui::GetIO();
    Scene& scene = m_engine->scene();
    const ViewProjector view = ViewProjector::from(m_engine->renderer().frameData().camera, {io.DisplaySize.x, io.DisplaySize.y});
    ImDrawList* list = ImGui::GetBackgroundDrawList();
    DebugDraw dd(list, view);
    const glm::vec2 mouse(io.MousePos.x, io.MousePos.y);

    // --- Icons for things you can't otherwise see or click ---
    struct Icon {
        glm::vec2 pos;
        uint64_t id;
    };
    std::vector<Icon> icons;
    if (m_showIcons) {
        for (const auto& o : scene.objects()) {
            if (o->isDestroyed() || o->hideInHierarchy || !o->activeInHierarchy()) continue;
            if (!m_showCodeObjects && !o->persistent) continue;
            const char* letter = nullptr;
            uint32_t color = 0;
            if (o->getComponent<PointLight>()) { letter = "L"; color = IM_COL32(255, 210, 80, 230); }
            else if (o->getComponent<DirectionalLight>()) { letter = "S"; color = IM_COL32(255, 240, 150, 230); }
            else if (o->getComponent<AudioZone>()) { letter = "Z"; color = IM_COL32(80, 200, 255, 230); }
            else if (o->getComponent<AudioSource>()) { letter = "A"; color = IM_COL32(230, 120, 255, 230); }
            else if (o->getComponent<Camera>()) { letter = "C"; color = IM_COL32(220, 220, 220, 230); }
            else if (o->persistent && o->components().empty() && o->transform().children().empty()) { letter = "o"; color = IM_COL32(200, 200, 200, 200); }
            if (!letter) continue;
            const glm::vec3 p = o->transform().position();
            if (glm::length(p - view.eye) > 120.0f) continue;
            const auto s = view.project(p);
            if (!s || s->x < 0 || s->y < 0 || s->x > view.size.x || s->y > view.size.y) continue;
            const bool sel = o->id() == m_selected;
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
        if (auto* l = sel->getComponent<PointLight>()) dd.sphere(glm::vec3(m[3]), l->range, IM_COL32(255, 210, 80, 120));
        if (auto* a = sel->getComponent<AudioSource>(); a && a->spatial) {
            dd.circle(glm::vec3(m[3]), {0, 1, 0}, a->minDistance, IM_COL32(230, 120, 255, 200));
            dd.circle(glm::vec3(m[3]), {0, 1, 0}, a->maxDistance, IM_COL32(230, 120, 255, 110));
        }

        ImDrawList* fg = ImGui::GetBackgroundDrawList();
        switch (m_gizmo.manipulate(fg, view, sel->transform(), m_gizmoOptions)) {
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

} // namespace ve
