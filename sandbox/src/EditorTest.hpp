#pragma once
// Scripted editor exercise (SAVANNA_EDITORTEST=1): picking, create/duplicate/
// delete, undo/redo, save, Play/Stop and reload - without mouse input.
#include <ve/scene/GameObject.hpp>
#include <ve/scene/ObjectRef.hpp>

namespace savanna {

class EditorTest : public ve::GameObject {
public:
    EditorTest() : GameObject("EditorTest") { runInEditor = true; hideInHierarchy = true; }
    void update(float dt) override;

private:
    int m_frame = 0;
    size_t m_persistentAtPlay = 0;
    uint64_t m_terrainVersion = 0;
    const void* m_foliageSystem = nullptr; // W4: vegetation must regenerate after a terrain edit
    std::string m_grassPass; // pass name (a new pass may reuse the old one's address)
    size_t m_bouldersNew = 0;
    float m_heightBefore = 0.0f; // terrain brush checks
    size_t m_editedTiles = 0;
    ve::ObjectRef<> m_markers; // model marker nodes check
};

} // namespace savanna
