#pragma once

// =============================================================================
// Mesh components that are described by data (so the editor can create them
// and scene files can store them):
//
//   ModelInstance      places an imported glTF model (by asset path); its nodes
//                      are spawned as (non-persistent) children when it starts.
//   PrimitiveRenderer  a cube / sphere / cylinder / plane with a simple PBR
//                      material - handy for blocking out levels.
//
// Both rebuild themselves when their fields change (editor) via rebuild().
// =============================================================================

#include "ve/scene/GameObject.hpp"
#include "ve/scene/ObjectRef.hpp"

#include <glm/glm.hpp>

#include <memory>
#include <string>

namespace ve {

class MeshRenderer;
class StandardMaterial;

class ModelInstance : public Component {
public:
    std::string path;             // e.g. "models/rondavel.gltf"
    bool staticColliders = false; // exact mesh colliders on every node, static renderers (buildings)
    std::string hiddenNodes;      // comma-separated node names to leave out (e.g. "Door")
    std::string animation;        // animated models: the clip to loop (empty = the first)

    void start() override { rebuild(); }
    void onDestroy() override;
    void rebuild();
    GameObject* instanceRoot() const; // null until started / if it was destroyed

private:
    ObjectRef<> m_root; // the editor or game may destroy the spawned nodes
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
    bool isStatic = false;              // never moves: its MeshRenderer is static (batched on the GPU)

    void start() override { rebuild(); }
    void onDestroy() override;
    void rebuild();

private:
    // The MeshRenderer this added next to itself (the editor may remove it).
    ComponentRef<MeshRenderer> m_renderer;
    std::shared_ptr<StandardMaterial> m_material;
};

} // namespace ve
