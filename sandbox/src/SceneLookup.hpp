#pragma once

// Finding the scene's one-of-a-kind objects (the Builder, the HUD, the
// Weather...) without walking the whole scene every frame: remember the id
// (findById is O(1) and can't dangle, unlike a pointer) and only search again
// once that object is gone.
//
//   Hud* hud = findCached<Hud>(scene(), m_hudId);

#include <ve/scene/Scene.hpp>

#include <cstdint>

namespace savanna {

template <class T>
T* findCached(const ve::Scene& scene, uint64_t& id)
{
    if (auto* t = dynamic_cast<T*>(scene.findById(id))) return t;
    T* t = scene.findFirst<T>();
    id = t ? t->id() : 0;
    return t;
}

} // namespace savanna
