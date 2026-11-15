#include "EditorTest.hpp"

#include <ve/core/Engine.hpp>
#include <ve/core/Log.hpp>
#include <ve/editor/Editor.hpp>
#include <ve/render/Renderer.hpp>
#include <ve/scene/Scene.hpp>
#include <ve/scene/SceneSerializer.hpp>

#include <cstdlib>
#include <filesystem>
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
    if (m_frame == 150 && std::string(std::getenv("SAVANNA_EDITORTEST")) != "hold") {
        log("done");
        engine().quit();
    }
}

} // namespace savanna
