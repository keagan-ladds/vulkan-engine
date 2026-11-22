#include "EditorTest.hpp"

#include "Props.hpp"

#include <ve/core/Engine.hpp>
#include <ve/core/Env.hpp>
#include <ve/core/Log.hpp>
#include <ve/editor/Editor.hpp>
#include <ve/gameplay/Placement.hpp>
#include <ve/render/Renderer.hpp>
#include <ve/scene/MeshComponents.hpp>
#include <ve/scene/Prefab.hpp>
#include <ve/scene/Scene.hpp>
#include <ve/scene/SceneSerializer.hpp>
#include <ve/world/Grass.hpp>
#include <ve/world/Terrain.hpp>
#include <ve/world/Vegetation.hpp>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace savanna {

namespace {
size_t countPersistent(const ve::Scene& s)
{
    size_t n = 0;
    for (const auto& o : s.objects()) n += (o->persistent && !o->isDestroyed()) ? 1 : 0;
    return n;
}
void log(const std::string& m) { ve::log::info("[editortest] {}", m); }
// The T on the object named `name` under `root`.
template <typename T>
T* componentOn(ve::GameObject& root, const std::string& name)
{
    if (root.name() == name) return root.getComponent<T>();
    for (ve::Transform* c : root.transform().children())
        if (T* t = componentOn<T>(c->gameObject(), name)) return t;
    return nullptr;
}
} // namespace

void EditorTest::update(float)
{
    ve::Editor* ed = engine().editor();
    if (!ed) return;
    ++m_frame;
    ve::Scene& s = scene();
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "ve_editortest_scene.json";

    if (m_frame == 5) ed->setMode(ve::Editor::Mode::Edit);
    if (m_frame == 30) {
        const VkExtent2D e = renderer().graph().get(renderer().sceneColor()).extent;
        ed->requestPick(glm::ivec2(e.width / 2, e.height * 3 / 4)); // ground in front of the camera
        log("pick requested");
    }
    if (m_frame == 45) log(std::string("picked: ") + (ed->selected() ? ed->selected()->name() : "(nothing)"));
    if (m_frame == 50) {
        ve::Json cube{{"type", "GameObject"}, {"name", "Test cube"}, {"position", {20.0f, 4.0f, 14.0f}},
                      {"components", ve::Json::array({{{"type", "PrimitiveRenderer"}}, {{"type", "Rigidbody"}, {"fields", {{"type", 0}, {"shape", 1}}}}})}};
        ve::GameObject* c = ed->createObject(cube, nullptr, "Create cube");
        ve::GameObject* d = ed->duplicate(*c);
        ed->deleteObject(*d);
        log("created + duplicated + deleted, persistent=" + std::to_string(countPersistent(s)));
    }
    if (m_frame == 55) {
        ed->undo(); // restores the duplicate
        log("after undo, persistent=" + std::to_string(countPersistent(s)));
    }
    if (m_frame == 60) {
        ed->redo();
        log("after redo, persistent=" + std::to_string(countPersistent(s)));
    }
    if (m_frame == 65) log(ed->saveScene(file) ? "saved " + file.string() : "SAVE FAILED");
    if (m_frame == 70) {
        m_persistentAtPlay = countPersistent(s);
        ed->setMode(ve::Editor::Mode::Play);
        log("play");
    }
    if (m_frame == 100) {
        ed->setMode(ve::Editor::Mode::Edit);
        log("stop");
    }
    if (m_frame == 105) log("after stop, persistent=" + std::to_string(countPersistent(s)) + " (at play " + std::to_string(m_persistentAtPlay) + ")");
    if (m_frame == 110) ed->openScene(file);
    if (m_frame == 115) log("after reopen, persistent=" + std::to_string(countPersistent(s)));
    if (m_frame == 120) {
        if (auto* o = s.find("Test cube")) ed->select(o);
        log(std::string("selected for inspector: ") + (ed->selected() ? ed->selected()->name() : "(none)"));
    }
    // Terrain: move the kopje stamp 40 m east; the terrain must rebuild there.
    auto boulders = [&](glm::vec2 p) -> size_t {
        auto* f = dynamic_cast<ve::Foliage*>(s.find("Foliage"));
        if (!f || !f->system() || f->layerIndex("Boulders") < 0) return 0;
        return f->system()->query(uint32_t(f->layerIndex("Boulders")), p, 25.0f).size();
    };
    if (m_frame == 125) {
        if (auto* f = dynamic_cast<ve::Foliage*>(s.find("Foliage"))) m_foliageSystem = f->system();
        if (auto* g = dynamic_cast<ve::GrassLayer*>(s.find("Grass")); g && g->pass()) m_grassPass = g->pass()->name();
        m_bouldersNew = boulders({160.0f, -80.0f});
        log("boulders near the kopje: " + std::to_string(boulders({120.0f, -80.0f})) + ", near the new spot " +
            std::to_string(m_bouldersNew));
        if (auto* t = ve::Terrain::find(s)) {
            m_terrainVersion = t->version();
            log("kopje height before: old " + std::to_string(t->height(120, -80)) + ", new spot " + std::to_string(t->height(160, -80)));
        }
        if (auto* k = s.find("Kopje")) {
            k->transform().setPosition(k->transform().position() + glm::vec3(40.0f, 0.0f, 0.0f));
            ed->select(k);
            if (ve::env::get("SAVANNA_EDITORTEST") == "hold") {
                // Look at it from above for a screenshot.
                if (auto* cam = s.find("Editor Camera")) {
                    cam->transform().setPosition({160.0f, 110.0f, 30.0f});
                }
            }
        }
    }
    if (m_frame == 145) {
        if (auto* t = ve::Terrain::find(s))
            log("terrain rebuilt: " + std::string(t->version() > m_terrainVersion ? "yes" : "NO") + ", old " +
                std::to_string(t->height(120, -80)) + ", new spot " + std::to_string(t->height(160, -80)));
    }
    // Vegetation follows (its own throttle): wait for both to rebuild, at most ~450 frames.
    if (m_frame > 145 && m_frame < 600 && m_foliageSystem) {
        auto* f = dynamic_cast<ve::Foliage*>(s.find("Foliage"));
        auto* g = dynamic_cast<ve::GrassLayer*>(s.find("Grass"));
        const bool foliageNew = f && f->system() && f->system() != m_foliageSystem;
        const bool grassNew = g && g->pass() && g->pass()->name() != m_grassPass;
        if ((foliageNew && grassNew) || m_frame == 599) {
            log(std::string("vegetation regenerated: foliage ") + (foliageNew ? "yes" : "NO") + ", grass " +
                (grassNew ? "yes" : "NO") + "; boulders near the old spot " + std::to_string(boulders({120.0f, -80.0f})) +
                ", near the new spot " + std::to_string(boulders({160.0f, -80.0f})) + " (was " +
                std::to_string(m_bouldersNew) + ")");
            m_foliageSystem = nullptr;
            m_frame = 600;
        }
    }
    // Terrain brushes: raise a hill and paint desert next to it (incremental
    // rebuild: foliage and grass must refresh, not be recreated), save, reload.
    const glm::vec2 hill(60.0f, 60.0f), sand(80.0f, 60.0f);
    if (m_frame == 605) {
        auto* t = ve::Terrain::find(s);
        auto* f = dynamic_cast<ve::Foliage*>(s.find("Foliage"));
        auto* g = dynamic_cast<ve::GrassLayer*>(s.find("Grass"));
        if (t && f && g && g->pass()) {
            m_foliageSystem = f->system();
            m_grassPass = g->pass()->name();
            m_terrainVersion = t->version();
            m_heightBefore = t->height(hill.x, hill.y);
            log("brush: height before " + std::to_string(m_heightBefore) + ", desert weight " +
                std::to_string(t->biomeWeight("Desert", sand.x, sand.y)));
            ve::Terrain::Brush raise;
            raise.radius = 8.0f;
            raise.strength = 1.0f;
            for (int i = 0; i < 30; ++i) t->applyBrush(raise, hill, 0.05f); // +12 m at the centre
            ve::Terrain::Brush paint;
            paint.mode = ve::Terrain::Brush::Mode::Paint;
            paint.radius = 6.0f;
            paint.strength = 1.0f;
            for (size_t b = 0; b < t->biomes.size(); ++b)
                if (t->biomes[b].name == "Desert") paint.biome = int(b);
            for (int i = 0; i < 10; ++i) t->applyBrush(paint, sand, 0.05f);
            ed->select(t);
            if (auto* cam = s.find("Editor Camera")) cam->transform().setPosition({hill.x, m_heightBefore + 25.0f, hill.y + 70.0f});
        }
    }
    if (m_frame == 636) {
        if (const auto shot = ve::env::get("SAVANNA_EDITORTEST_SHOT")) renderer().requestCapture(std::string(*shot)); // the brushed hill
    }
    if (m_frame == 640) {
        auto* t = ve::Terrain::find(s);
        auto* f = dynamic_cast<ve::Foliage*>(s.find("Foliage"));
        auto* g = dynamic_cast<ve::GrassLayer*>(s.find("Grass"));
        if (t && f && g) {
            m_editedTiles = t->editedTiles();
            log("brush: rebuilt " + std::string(t->version() > m_terrainVersion ? "yes" : "NO") + ", height after " +
                std::to_string(t->height(hill.x, hill.y)) + " (+" + std::to_string(t->height(hill.x, hill.y) - m_heightBefore) +
                "), desert weight " + std::to_string(t->biomeWeight("Desert", sand.x, sand.y)) + ", edited tiles " +
                std::to_string(m_editedTiles));
            log(std::string("brush: foliage kept its system (refreshed in place) ") + (f->system() == m_foliageSystem ? "yes" : "NO") +
                ", grass kept its pass " + (g->pass() && g->pass()->name() == m_grassPass ? "yes" : "NO") + ", collider tiles " +
                std::to_string(t->colliderTiles()));
        }
        m_foliageSystem = nullptr;
        log(ed->saveScene(file) ? "brush: saved" : "brush: SAVE FAILED");
    }
    if (m_frame == 645) ed->openScene(file);
    if (m_frame == 680) {
        if (auto* t = ve::Terrain::find(s))
            log("brush: after reopen, edited tiles " + std::to_string(t->editedTiles()) + " (saved " + std::to_string(m_editedTiles) +
                "), height " + std::to_string(t->height(hill.x, hill.y)) + ", desert weight " +
                std::to_string(t->biomeWeight("Desert", sand.x, sand.y)));
    }
    if (m_frame == 682) {
        // Prefabs: save a configured object, make a new one from the file.
        const std::string path = (std::filesystem::temp_directory_path() / "ve_editortest_torch.prefab").string();
        auto* torch = dynamic_cast<Torch*>(s.find("Torch"));
        if (torch) {
            torch->intensity = 9.25f;
            const bool saved = ve::prefab::save(*torch, path);
            ve::prefab::clearCache(); // read it back from the file
            ve::GameObject* copy = ve::prefab::instantiate(s, path, {5.0f, 3.0f, 5.0f});
            auto* t2 = dynamic_cast<Torch*>(copy);
            const auto* link = copy ? copy->getComponent<ve::PrefabInstance>() : nullptr;
            log(std::string("prefab: saved ") + (saved ? "yes" : "NO") + ", instance is a Torch " + (t2 ? "yes" : "NO") + ", intensity " +
                (t2 ? std::to_string(t2->intensity) : std::string("?")) + ", new id " + (copy && copy->id() != torch->id() ? "yes" : "NO") +
                ", linked to the file " + (link && link->prefab == path ? "yes" : "NO"));
            std::filesystem::remove(path);
        } else {
            log("prefab: no Torch in the scene");
        }
    }
    if (m_frame == 683) {
        // Model markers: SNAP / BOX / EXCL nodes in a glTF become components.
        const std::string path = (std::filesystem::temp_directory_path() / "ve_editortest_markers.gltf").string();
        ve::Json g{{"asset", {{"version", "2.0"}}}, {"scene", 0}, {"scenes", {{{"nodes", {0}}}}}};
        g["nodes"] = ve::Json::array({
            {{"name", "Piece"}, {"children", {1, 2, 3, 4, 5, 6, 7}}},
            {{"name", "SNAP"}, {"translation", {1, 0, 0}}},
            {{"name", "SNAP_fence_fence+grid.002"}, {"translation", {0, 1, 0}}},
            {{"name", "SNAP_door_door-opening_align"}, {"rotation", {0, 0.7071068, 0, 0.7071068}}, {"extras", {{"accepts", "*"}}}},
            {{"name", "BOX"}, {"translation", {0, 1, 0}}, {"scale", {2, 1, 0.5}}},
            {{"name", "EXCL_circle"}, {"scale", {3, 1, 3}}, {"extras", {{"grass", false}}}},
            {{"name", "BOX_mesh"}, {"mesh", 0}, {"scale", {2, 2, 2}}},
            {{"name", "SNAPSHOT"}},
        });
        // One triangle (0,0,0) (1,0,0) (0,2,1).
        g["buffers"] = {{{"byteLength", 36}, {"uri", "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAAEAAAIA/"}}};
        g["bufferViews"] = {{{"buffer", 0}, {"byteLength", 36}}};
        g["accessors"] = {{{"bufferView", 0}, {"componentType", 5126}, {"count", 3}, {"type", "VEC3"}, {"min", {0, 0, 0}}, {"max", {1, 2, 1}}}};
        g["meshes"] = {{{"primitives", {{{"attributes", {{"POSITION", 0}}}}}}}};
        std::ofstream(path) << g.dump();

        ve::GameObject& o = s.spawn("Markers test");
        o.transform().setPosition({30.0f, 0.0f, 30.0f});
        o.addComponent<ve::ModelInstance>().path = path;
        m_markers = &o;
    }
    if (m_frame == 686) {
        if (ve::GameObject* found = m_markers.get(s)) {
            ve::GameObject& o = *found;
            auto near = [](glm::vec3 a, glm::vec3 b) { return glm::length(a - b) < 1e-3f; };
            const auto* plain = componentOn<ve::SnapPoint>(o, "SNAP");
            const auto* fence = componentOn<ve::SnapPoint>(o, "SNAP_fence_fence+grid.002");
            const auto* door = componentOn<ve::SnapPoint>(o, "SNAP_door_door-opening_align");
            const auto* box = componentOn<ve::PlacementBox>(o, "BOX");
            const auto* excl = componentOn<ve::FoliageExclusion>(o, "EXCL_circle");
            const auto* meshBox = componentOn<ve::PlacementBox>(o, "BOX_mesh");
            const bool meshHidden = !componentOn<ve::MeshRenderer>(o, "BOX_mesh");
            const bool notMarker = !componentOn<ve::SnapPoint>(o, "SNAPSHOT");
            const bool ok = plain && plain->type == "grid" && plain->accepts == "grid" && near(plain->worldPosition(), {31, 0, 30}) &&
                            fence && fence->type == "fence" && fence->accepts == "fence, grid" && !fence->alignRotation && door &&
                            door->type == "door" && door->accepts == "*" && door->alignRotation && near(door->worldFacing(), {1, 0, 0}) &&
                            box && near(box->halfExtents, {2, 1, 0.5f}) && near(box->center, {0, 0, 0}) && excl && !excl->box &&
                            std::abs(excl->radius - 3.0f) < 1e-3f && !excl->grass && excl->foliage && meshBox &&
                            near(meshBox->center, {1, 2, 1}) && near(meshBox->halfExtents, {1, 2, 1}) && meshHidden && notMarker &&
                            ve::placement::snapPointsOf(o).size() == 3 && ve::placement::volumeOf(o).size() == 2;
            log(std::string("markers: ") + (ok ? "ok" : "WRONG") + " (snap points " + std::to_string(ve::placement::snapPointsOf(o).size()) +
                ", boxes " + std::to_string(ve::placement::volumeOf(o).size()) + ")");
            o.destroy();
        } else {
            log("markers: WRONG (the test object is gone)");
        }
        m_markers.reset();
        std::filesystem::remove(std::filesystem::temp_directory_path() / "ve_editortest_markers.gltf");
    }
    if (m_frame == 690 && ve::env::get("SAVANNA_EDITORTEST") != "hold") {
        log("done");
        engine().quit();
    }
}

} // namespace savanna
