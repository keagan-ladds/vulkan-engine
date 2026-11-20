// Undo / redo: per-object JSON snapshots, applying them, the cached "before"
// state of one-frame inspector edits, the undo-able structural operations and
// restoring the scene when Play stops. See Editor.hpp.

#include "ve/editor/Editor.hpp"

#include "ve/core/Engine.hpp"
#include "ve/editor/EditorInternal.hpp"
#include "ve/physics/Physics.hpp"
#include "ve/scene/Scene.hpp"
#include "ve/scene/SceneSerializer.hpp"

#include <functional>
#include <set>

namespace ve {

using namespace editor_internal;

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
    m_editBeforeValid = false;
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
    m_editBeforeValid = false; // something changed
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
    m_editBeforeValid = false; // (even with no change: the edit may have been reverted in place)
    ObjectState after = snapshot(m_editBefore.id);
    if (after.json == m_editBefore.json && after.parent == m_editBefore.parent) return;
    pushUndo({label, {m_editBefore}, {after}});
}

void Editor::refreshEditBefore(const GameObject& selection, float dt)
{
    // Cached (see m_editBefore): re-serializing the subtree every frame is costly.
    m_editBeforeAge += dt;
    if (!m_editing && (!m_editBeforeValid || m_editBefore.id != selection.id() || m_editBeforeAge > 1.0f)) {
        m_editBefore = snapshot(selection.id());
        m_editBeforeValid = true;
        m_editBeforeAge = 0.0f;
    }
}

void Editor::afterTransformEdit(GameObject& object)
{
    // Physics bodies don't follow Transform edits by themselves (static ones never move).
    forSubtree(object, [](GameObject& g) {
        for (const auto& c : g.components()) {
            if (auto* rb = dynamic_cast<Rigidbody*>(c.get()); rb && !rb->isRemoved()) rb->rebuild();
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
    if (object.id() == m_camera.id()) return;
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
    if (m_selected.id() == object.id()) select(nullptr);
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

void Editor::restoreSnapshot(const Json& snapshot)
{
    // In place where possible: objects that still exist get their saved state
    // re-applied (game code may hold pointers to them, e.g. the terrain); only
    // objects destroyed during play are re-created, and ones created during
    // play are removed.
    Scene& scene = m_engine->scene();
    std::set<uint64_t> keep;
    std::function<void(const Json&)> collectIds = [&](const Json& j) {
        keep.insert(SceneSerializer::idFromString(j.value("id", std::string())));
        if (auto ch = j.find("children"); ch != j.end())
            for (const Json& c : *ch) collectIds(c);
    };
    std::function<void(const Json&, GameObject*)> apply = [&](const Json& j, GameObject* parent) {
        const uint64_t id = SceneSerializer::idFromString(j.value("id", std::string()));
        keep.insert(id);
        GameObject* o = scene.findById(id);
        if (o && !o->isDestroyed() && SceneSerializer::applyObject(*o, j)) {
            const Transform* want = parent ? &parent->transform() : o->transform().parent();
            if (o->transform().parent() != want) o->setParent(parent, false);
            afterTransformEdit(*o);
            if (auto ch = j.find("children"); ch != j.end())
                for (const Json& c : *ch) apply(c, o);
        } else {
            if (o && !o->isDestroyed()) o->destroy();
            SceneSerializer::createObject(scene, j, parent, false);
            collectIds(j);
        }
    };
    m_editBeforeValid = false;
    if (auto objects = snapshot.find("objects"); objects != snapshot.end())
        for (const Json& oj : *objects) apply(oj, nullptr);
    std::vector<GameObject*> extra;
    for (const auto& o : scene.objects()) {
        if (o->persistent && !o->isDestroyed() && !keep.count(o->id())) extra.push_back(o.get());
    }
    for (GameObject* o : extra) o->destroy();
}

} // namespace ve
