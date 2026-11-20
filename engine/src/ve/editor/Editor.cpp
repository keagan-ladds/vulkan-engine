#include "ve/editor/Editor.hpp"

// The editor's code is split by panel (Editor.hpp lists which file holds what):
//   Editor.cpp          - lifecycle, modes, per-frame hooks, shortcuts, menu bar, scene files
//   EditorHierarchy.cpp - the hierarchy panel and the Create menu
//   EditorInspector.cpp - the inspector and its field widgets
//   EditorUndo.cpp      - snapshots, undo / redo and the undo-able operations
//   EditorViewport.cpp  - editor camera, picking, icons, gizmo, terrain brush
// Helpers they share are in EditorInternal.hpp.

#include "ve/assets/AssetManager.hpp"
#include "ve/core/Engine.hpp"
#include "ve/core/Input.hpp"
#include "ve/editor/EditorInternal.hpp"
#include "ve/editor/PickingPass.hpp"
#include "ve/render/Renderer.hpp"
#include "ve/scene/Components.hpp"
#include "ve/scene/Prefab.hpp"
#include "ve/scene/Scene.hpp"
#include "ve/scene/SceneSerializer.hpp"
#include "ve/ui/UiLayer.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <sstream>

namespace ve {

using namespace editor_internal;

namespace {

GameObject* persistentAncestorOrRoot(GameObject* o)
{
    GameObject* root = o;
    for (GameObject* g = o; g; g = g->transform().parent() ? &g->transform().parent()->gameObject() : nullptr) {
        if (g->persistent) return g;
        root = g;
    }
    return root;
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
    m_camera.reset();
    m_picking = nullptr;
}

void Editor::setMode(Mode mode)
{
    // Before the game has run a couple of frames its camera isn't placed yet:
    // apply the switch once it is (e.g. a game that starts in the editor).
    if (m_frames < 3 || m_engine->loadStepsPending()) {
        m_pendingMode = mode;
        m_hasPending = true;
        return;
    }
    if (mode == m_mode) return;
    Scene& scene = m_engine->scene();
    Input& in = m_engine->input();

    if (m_mode == Mode::Play && mode != Mode::Play && !m_playSnapshot.is_null()) {
        // Stop: every persistent object back to how it was when Play was pressed.
        restoreSnapshot(m_playSnapshot);
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
            if (c.isActive() && c.gameObject().id() != m_camera.id() && (!game || c.priority > game->priority)) game = &c;
        });
        GameObject& cam = ensureCamera();
        if (m_mode == Mode::Off && game) {
            // Start where the game camera is.
            const glm::vec3 f = game->transform().forward();
            cam.transform().setPosition(game->transform().position());
            m_camYaw = std::atan2(-f.x, -f.z);
            m_camPitch = std::asin(glm::clamp(f.y, -1.0f, 1.0f));
        }
        cam.setActive(true);
    } else if (GameObject* cam = camera()) {
        cam->setActive(false);
        m_looking = false;
    }
    m_mode = mode;
    m_editBeforeValid = false;
}

GameObject* Editor::selected() const
{
    return m_selected.get(m_engine->scene()); // (null once destroyed)
}

void Editor::select(GameObject* object)
{
    m_selected = object;
    m_editBeforeValid = false;
}

// -----------------------------------------------------------------------------
// Frame
// -----------------------------------------------------------------------------

void Editor::beginFrame(float dt)
{
    Input& in = m_engine->input();
    if (m_frames < 3) ++m_frames;
    // A pending switch waits for the game's camera: a few frames, and the level's load steps.
    if (m_hasPending && m_frames >= 3 && !m_engine->loadStepsPending()) {
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
        GameObject* cam = camera();
        if (ImGui::IsKeyPressed(ImGuiKey_F) && sel && cam) {
            AABB b;
            glm::vec3 c = sel->transform().position();
            float r = 2.0f;
            if (subtreeBounds(*sel, b)) {
                c = b.center();
                r = std::max(glm::length(b.extents()), 0.5f);
            }
            cam->transform().setPosition(c - cam->transform().forward() * (r * 2.5f + 1.0f));
        }
    }
}

void Editor::onGui()
{
    if (m_engine->loadingInEditor()) drawLoading();
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
    if (m_openSavePrefab) {
        ImGui::OpenPopup("Save as prefab");
        m_openSavePrefab = false;
    }
    if (ImGui::BeginPopupModal("Save as prefab", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Prefab path (relative to the assets folder):");
        ImGui::SetNextItemWidth(360.0f);
        const bool enter = ImGui::InputText("##prefab", m_prefabBuffer, sizeof(m_prefabBuffer), ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::Button("Save") || enter) {
            if (GameObject* sel = selected()) {
                const bool ok = prefab::save(*sel, m_prefabBuffer);
                m_status = ok ? std::string("Saved prefab ") + m_prefabBuffer : "Prefab: can't write the file";
                m_statusTime = 3.0f;
                m_assetCacheAge = 1e9f; // list it in Create > Prefab
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
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
    if (m_engine->loading()) return; // one at a time
    // Through the engine's loading: terrain, foliage and navmeshes register
    // load tasks, and the progress shows in the editor (drawLoading) while
    // the world fills in.
    select(nullptr);
    m_engine->beginLoading(nullptr);
    m_engine->addLoadStep("Opening " + path.filename().generic_string(), [this, path] {
        SceneSerializer::clearPersistent(m_engine->scene());
        SceneSerializer::load(m_engine->scene(), path);
        m_scenePath = path.generic_string();
        m_undo.clear();
        m_redo.clear();
        m_status = "Opened " + m_scenePath;
        m_statusTime = 3.0f;
    });
}

void Editor::drawLoading()
{
    // A small panel at the bottom centre of the viewport: what's loading, how far.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y - 48.0f}, ImGuiCond_Always, {0.5f, 1.0f});
    ImGui::SetNextWindowBgAlpha(0.85f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##editorLoading", nullptr, flags)) {
        static const char spin[] = "|/-\\";
        const int frame = int(ImGui::GetTime() * 8.0) % 4;
        ImGui::Text("%c  Loading: %s", spin[frame], m_engine->loadStatus().c_str());
        ImGui::ProgressBar(m_engine->loadProgress(), {320.0f, 0.0f});
    }
    ImGui::End();
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
// Menu bar / toolbars
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
        ImGui::Separator();
        if (ImGui::MenuItem("Save as prefab...", nullptr, false, sel && sel->persistent)) {
            std::string name = sel->name();
            for (char& c : name) c = std::isalnum(static_cast<unsigned char>(c)) ? char(std::tolower(static_cast<unsigned char>(c))) : '_';
            std::snprintf(m_prefabBuffer, sizeof(m_prefabBuffer), "prefabs/%s.prefab", name.c_str());
            m_openSavePrefab = true;
        }
        ImGui::EndMenu();
    }
    drawCreateMenu();
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Show code objects", nullptr, &m_showCodeObjects);
        ImGui::MenuItem("Show icons", nullptr, &m_showIcons);
        ImGui::MenuItem("Engine overlay (F1)", nullptr, &m_engine->ui().debugOverlay);
        ImGui::EndMenu();
    }

    // Toolbar.
    ImGui::Separator();
    ImGui::BeginDisabled(m_engine->loading()); // not before the world is ready
    if (ImGui::Button("Play")) setMode(Mode::Play);
    ImGui::EndDisabled();
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
    ImGui::Checkbox("Snap points", &m_snapToPoints);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Moving an object with SnapPoints snaps it onto other objects' points");
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
}

} // namespace ve
