#pragma once

// Finding the scene's one-of-a-kind objects (the Builder, the HUD, the
// Weather...) without walking the whole scene every frame: remember it as an
// ObjectRef (an id: O(1) to look up and can't dangle, unlike a pointer) and
// only search again once that object is gone.
//
//   ve::ObjectRef<Hud> m_hud;
//   Hud* hud = findCached(scene(), m_hud);

#include <ve/scene/ObjectRef.hpp>
#include <ve/scene/Scene.hpp>

namespace savanna {

template <class T>
T* findCached(const ve::Scene& scene, ve::ObjectRef<T>& ref)
{
    if (T* t = ref.get(scene)) return t;
    T* t = scene.findFirst<T>();
    ref = t;
    return t;
}

} // namespace savanna
