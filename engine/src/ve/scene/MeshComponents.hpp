#pragma once

// =============================================================================
// Mesh components that are described by data (so the editor can create them
// and scene files can store them):
//
//   ModelInstance      places an imported glTF model (by asset path); its nodes
//                      are spawned as (non-persistent) children when it starts.
//                      Marker nodes (usually Blender empties) become components
//                      instead of being drawn (`markers`, on by default). The
//                      engine registers (registerModelMarker):
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

#include "ve/core/Log.hpp"
#include "ve/render/Geometry.hpp"
#include "ve/scene/GameObject.hpp"
#include "ve/scene/ObjectRef.hpp"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ve {

class MeshRenderer;
class StandardMaterial;
class Model;
struct ModelNode;

/// A marker node, as a marker handler sees it: a model node named KIND or
/// KIND_<args> (KIND registered with registerModelMarker; Blender's ".001"
/// duplicate suffix ignored). Its object's components were removed first: a
/// marker isn't drawn or solid, its mesh only gives its size.
struct ModelMarker {
    GameObject& object;            ///< the node's object (a non-persistent child of the ModelInstance)
    const ModelNode& node;  ///< The glTF node the marker came from.
    const Model& model;  ///< The model the node belongs to.
    std::vector<std::string> args; ///< "SNAP_fence_fence+grid" -> {"fence", "fence+grid"}
    AABB bounds;                   ///< the node mesh's local bounds, else a cube empty's (-1..1)
    nlohmann::json extras;         ///< glTF extras (Blender custom properties); an empty object if none

    /// extras[key] if present and of T's type, else `fallback` (with a warning on a wrong type).
    template <class T>
    T extra(const char* key, T fallback) const
    {
        const auto it = extras.find(key);
        if (it == extras.end()) return fallback;
        try {
            return it->template get<T>();
        } catch (const nlohmann::json::exception&) {
            log::warn("Model marker '{}': custom property '{}' has the wrong type", object.name(), key);
            return fallback;
        }
    }
};
using ModelMarkerHandler = std::function<void(ModelMarker&)>;

/// Lets a module turn marker nodes named `kind` (upper case by convention,
/// e.g. "SNAP") into its components. Called once at startup by the module's
/// registration (gameplay: SNAP, BOX; world: EXCL), so scene code needn't know
/// those modules. A later call for the same kind replaces the handler.
void registerModelMarker(const std::string& kind, ModelMarkerHandler handler);

/// Spawns a model file's node hierarchy (glTF) under its object and keeps it
/// in sync with `path`: the scene-file way of placing a model.
class ModelInstance : public Component {
public:
    std::string path;             ///< e.g. "models/rondavel.gltf"
    bool staticColliders = false; ///< exact mesh colliders on every node, static renderers (buildings)
    std::string hiddenNodes;      ///< comma-separated node names to leave out (e.g. "Door")
    std::string animation;        ///< animated models: the clip to loop (empty = the first)
    bool markers = true;          ///< marker nodes (SNAP_*, BOX_*, EXCL_*...) become components (see above)

    void start() override { rebuild(); }
    void onDestroy() override;
    /// Respawns the model's nodes from the fields (the editor calls it after edits; code after changing fields).
    void rebuild();
    GameObject* instanceRoot() const; ///< null until started / if it was destroyed

private:
    ObjectRef<> m_root; ///< the editor or game may destroy the spawned nodes
};

/// Draws a simple shape (cube, sphere, cylinder, plane) with a plain material: blockouts, test props.
class PrimitiveRenderer : public Component {
public:
    /// The shapes available.
    enum class Shape { Cube, Sphere, Cylinder, Plane };
    Shape shape = Shape::Cube;  ///< Which shape.
    glm::vec3 size{1.0f};               ///< full extents (sphere/cylinder: size.x = diameter)
    glm::vec4 color{0.8f, 0.8f, 0.8f, 1.0f};  ///< Base colour (linear RGBA; alpha < 1 doesn't make it transparent).
    float roughness = 0.8f;  ///< 0 = mirror-like, 1 = fully rough.
    float metallic = 0.0f;  ///< 0 = dielectric, 1 = metal.
    std::string texture;                ///< optional albedo texture path
    glm::vec2 uvScale{1.0f};  ///< Texture repeats per unit of size (with `texture`).
    bool castShadows = true;  ///< Draw into shadow maps.
    bool isStatic = false;              ///< never moves: its MeshRenderer is static (batched on the GPU)

    void start() override { rebuild(); }
    void onDestroy() override;
    /// Recreates the mesh and material from the fields (after changing them from code).
    void rebuild();

private:
    /// The MeshRenderer this added next to itself (the editor may remove it).
    ComponentRef<MeshRenderer> m_renderer;
    std::shared_ptr<StandardMaterial> m_material;
};

} // namespace ve
