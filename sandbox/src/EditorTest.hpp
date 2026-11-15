#pragma once
// Scripted editor exercise (SAVANNA_EDITORTEST=1): picking, create/duplicate/
// delete, undo/redo, save, Play/Stop and reload - without mouse input.
#include <ve/scene/GameObject.hpp>

namespace savanna {

class EditorTest : public ve::GameObject {
public:
    EditorTest() : GameObject("EditorTest") { runInEditor = true; hideInHierarchy = true; }
    void update(float dt) override;

private:
    int m_frame = 0;
    size_t m_persistentAtPlay = 0;
    uint64_t m_terrainVersion = 0;
};

} // namespace savanna
