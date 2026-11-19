#pragma once

// =============================================================================
// Mesh components that are described by data (so the editor can create them
// and scene files can store them):
//
//   ModelInstance      places an imported glTF model (by asset path); its nodes
//                      are spawned as (non-persistent) children when it starts.
//                      Marker nodes (usually Blender empties) become components
//                      instead of being drawn (`markers`, on by default):
//                        SNAP[_type[_accepts]][_align]  SnapPoint at the node
//                            (accepts: `+`-separated, `any` = *, `none` = nothing;
//                            default: the type; no type = grid; facing = node +Z)
//                        BOX[_name]                     PlacementBox
//                        EXCL[_circle]                  FoliageExclusion (box, or circle)
//                      Size: a cube empty's (-1..1 times its scale), or the node's
//                      mesh bounds. Blender's ".001" suffixes are ignored; glTF
//                      extras (custom properties) override fields: type, accepts,
//                      align; falloff, grass, foliage, radius.
//   PrimitiveRenderer  a cube / sphere / cylinder / plane with a simple PBR
//                      material - handy for blocking out levels.
//
// Both rebuild themselves when their fields change (editor) via rebuild().
// =============================================================================

#include "ve/scene/GameObject.hpp"

#include <glm/glm.hpp>

#include <memory>
#include <string>

namespace ve {

class MeshRenderer;
class StandardMaterial;

class ModelInstance : public Component {
public:
    std::string path;             // e.g. "models/rondavel.gltf"
    bool staticColliders = false; // exact mesh colliders on every node (buildings)
    std::string hiddenNodes;      // comma-separated node names to leave out (e.g. "Door")
    std::string animation;        // animated models: the clip to loop (empty = the first)
    bool markers = true;          // SNAP / BOX / EXCL nodes become components (see above)

    void start() override { rebuild(); }
    void onDestroy() override;
    void rebuild();
    GameObject* instanceRoot() const { return m_root; }

private:
    GameObject* m_root = nullptr;
};

class PrimitiveRenderer : public Component {
public:
    enum class Shape { Cube, Sphere, Cylinder, Plane };
    Shape shape = Shape::Cube;
    glm::vec3 size{1.0f};               // full extents (sphere/cylinder: size.x = diameter)
    glm::vec4 color{0.8f, 0.8f, 0.8f, 1.0f};
    float roughness = 0.8f;
    float metallic = 0.0f;
    std::string texture;                // optional albedo texture path
    glm::vec2 uvScale{1.0f};
    bool castShadows = true;

    void start() override { rebuild(); }
    void onDestroy() override;
    void rebuild();

private:
    MeshRenderer* m_renderer = nullptr;
    std::shared_ptr<StandardMaterial> m_material;
};

} // namespace ve
