#pragma once

// Private helpers shared by the editor's source files (Editor*.cpp). Not part
// of the engine's API: only the editor includes this.

#include "ve/render/Geometry.hpp"
#include "ve/render/Mesh.hpp"
#include "ve/scene/Components.hpp"
#include "ve/scene/GameObject.hpp"

namespace ve::editor_internal {

// Panel layout (the menu bar on top, hierarchy left, inspector right).
inline constexpr float kMenuHeight = 22.0f;
inline constexpr float kHierarchyWidth = 290.0f;
inline constexpr float kInspectorWidth = 370.0f;

template <class F>
void forSubtree(GameObject& o, F&& fn)
{
    fn(o);
    for (Transform* c : o.transform().children()) forSubtree(c->gameObject(), fn);
}

// The world bounds of every mesh in an object's subtree (false: it has none).
inline bool subtreeBounds(GameObject& o, AABB& out)
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

} // namespace ve::editor_internal
