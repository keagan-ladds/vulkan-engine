#pragma once

// =============================================================================
// DecalProjector - a decal placed in the scene (see render/Decals.hpp for what
// a decal is and does). The box is the object's transform: it projects along
// forward() (-Z), and its scale is the decal's size (x = width, y = height,
// z = projection depth). Moves, rotates and scales with its object.
//
//   auto& go = scene.spawn<ve::GameObject>("Puddle");
//   go.transform().setPosition({4, 0, 2});
//   go.transform().setLocalEuler({-90, 0, 0});        // project straight down
//   go.transform().setLocalScale({3, 2, 0.5f});        // 3 x 2 m, 0.5 m deep
//   auto& d = go.addComponent<ve::DecalProjector>();
//   d.mask = "textures/decals/puddle_mask.png";        // A = the puddle's shape
//   d.color = {0.55f, 0.55f, 0.55f, 1}; d.blend = ve::DecalBlend::Multiply;   // wet = darker
//   d.roughness = 0.04f; d.roughnessOpacity = 1; d.flatten = 1;               // smooth, flat water
//
// Textures come from asset paths (scene files, editor; colour / data / normal
// is each image's TextureSettings: name them *_normal, *_mask or add a
// .texture file), or set the Decal texture fields from code (leave the path
// empty then). Changing a path from code: call reload().
// =============================================================================

#include "ve/render/Decals.hpp"
#include "ve/scene/GameObject.hpp"

#include <string>

namespace ve {

class DecalProjector : public Component, public Decal {
public:
    std::string albedo; // asset paths; empty = keep whatever texture code set
    std::string normal;
    std::string mask;

    void start() override { reload(); }
    // Loads the textures named by the paths that changed since the last load.
    void reload();

private:
    std::string m_loaded[3]; // paths the current textures came from
};

} // namespace ve
