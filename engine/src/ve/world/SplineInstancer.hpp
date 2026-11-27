#pragma once

// =============================================================================
// SplineInstancer - repeats a mesh along a spline: fences, walls of posts,
// powerline poles, street lights, crop rows, kerb stones. Optional cables /
// rails strung between consecutive items (powerlines sag, fence rails don't).
//
//   Create > Spline > Fence / Powerline in the editor, or:
//   auto& fence = scene.spawn<ve::SplineInstancer>();
//   fence.addComponent<ve::Spline>().setPoints({...});
//   fence.mesh = "models/fence_post.gltf";
//   fence.spacing = 2.5f;
//   fence.cableAttach = "0,0.9,0; 0,0.4,0";   // two rails, in the post's local space
//
// Items are GPU instances (the mesh's LOD chain, impostor and variants as for
// FoliageType; variants are picked at random per item), dropped onto the
// terrain unless followGround is off. Item axes: +X along the path, +Y up,
// +Z to the right of the direction of travel. Everything is rebuilt when the
// spline, the fields or the terrain change (live while you drag points).
//
// To keep vegetation off the line, give the object a FoliageExclusion with
// `spline` on (the editor's presets do): trees and grass clear within its
// width (docs/06-world.md, "Splines").
// =============================================================================

#include "ve/render/Instancing.hpp"
#include "ve/scene/GameObject.hpp"

#include <glm/glm.hpp>

#include <memory>
#include <string>
#include <vector>

namespace ve {

class Spline;

/// Repeats a mesh along the Spline on this object (see the top of this file).
class SplineInstancer : public GameObject {
public:
    SplineInstancer();
    ~SplineInstancer() override;
    void start() override;
    void update(float dt) override;
    void onDestroy() override;

    // ---- What ----
    std::string mesh;              ///< glTF model (LOD chain *_LOD0.., variants, .model cooking); empty = nothing (cables only)
    bool impostor = false;         ///< An octahedral impostor as the last LOD (far rows of poles).
    float viewDistance = 600.0f;   ///< Items aren't drawn beyond this (m).
    float shadowDistance = 200.0f; ///< ...and cast no shadows beyond this (m).
    bool castShadows = true;       ///< Items cast shadows.
    bool collision = false;        ///< A static box collider per item (LOD0 bounds): solid fences.

    // ---- Where ----
    float spacing = 3.0f;          ///< Metres between items along the path.
    bool atPoints = false;         ///< One item at each control point instead (powerline poles where you put them).
    bool fitEnds = true;           ///< Stretch the spacing a little so items land on both ends (open paths).
    float startOffset = 0.0f;      ///< The first item this far along (m).
    glm::vec2 offset{0.0f};        ///< Item offset: x to the right of the path, y up (m).
    bool followGround = true;      ///< Drop items onto the terrain (else they sit on the spline).
    bool tiltWithGround = false;   ///< Tilt items with the ground's slope (else they stand upright).
    bool alignToPath = true;       ///< Turn items to the path's direction (else world-aligned).
    float yaw = 0.0f;              ///< Extra rotation about Y (degrees): 90 = items face across the path.
    float randomYaw = 0.0f;        ///< Random extra rotation, +- this (degrees).
    glm::vec2 scaleRange{1.0f, 1.0f}; ///< Uniform scale, random in [x, y].
    float jitter = 0.0f;           ///< Random sideways offset, +- this (m).
    int seed = 1;                  ///< Seed of the random choices (variant, yaw, scale, jitter).

    // ---- Cables / rails between consecutive items ----
    /// Attachment points in the item's local space, "x,y,z; x,y,z" (empty = no cables).
    /// One cable per point, from each item to the next (closed splines wrap).
    std::string cableAttach;
    float cableSag = 0.0f;         ///< Sag at mid-span as a fraction of the span (0 = straight rails; ~0.03 powerlines).
    float cableRadius = 0.02f;     ///< Cable thickness (radius, m).
    glm::vec3 cableColor{0.08f, 0.08f, 0.08f}; ///< Cable colour (linear).
    int cableSides = 5;            ///< Sides of the cable tube (3..12).

    /// Rebuild everything (the inspector calls it after an edit; call it after changing fields from code).
    void markDirty() { m_meshDirty = m_dirty = true; }
    /// Item transforms as placed (world space).
    const std::vector<glm::mat4>& items() const { return m_items; }
    /// Cables strung (spans x attachment points).
    size_t cableCount() const { return m_cableCount; }

private:
    void rebuild();
    void buildMeshes();
    void clearBodies();
    std::vector<std::vector<InstanceLod>> m_variants;      ///< LOD chain per variant
    std::vector<glm::vec3> m_halfExtents, m_centres;      ///< LOD0 bounds per variant (colliders)
    std::vector<std::shared_ptr<InstanceGroup>> m_groups; ///< per variant
    std::shared_ptr<InstanceGroup> m_cables;
    std::vector<glm::mat4> m_items;
    std::vector<uint32_t> m_bodies;
    size_t m_cableCount = 0;
    uint64_t m_splineVersion = 0;
    uint64_t m_terrainVersion = 0;
    bool m_dirty = true;
    bool m_meshDirty = true;
};

class TypeRegistry;
/// Registers SplineInstancer with reflection (the Engine does at startup).
void registerSplineInstancerTypes(TypeRegistry& r);

} // namespace ve
