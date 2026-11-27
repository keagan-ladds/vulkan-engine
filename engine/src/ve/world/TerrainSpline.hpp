#pragma once

// =============================================================================
// TerrainSpline - a road, path, track or riverbed laid along a spline: it
// shapes the terrain under it (flat across its width, blending back into the
// land over `falloff`), paints a biome along it, and can lay a surface mesh
// with its own material (asphalt, gravel, planks) for edges sharper than the
// terrain's 2 m samples.
//
//   Create > Spline > Road / Path in the editor, or:
//   auto& road = scene.spawn<ve::TerrainSpline>();
//   road.addComponent<ve::Spline>().setPoints({...});
//   road.width = 5.0f;          // flat surface
//   road.biome = "Dirt";        // painted under it (a Terrain biome's name)
//   road.material = "materials/gravel.material"; // optional surface mesh
//
// The Terrain applies every active TerrainSpline after its generator, stamps
// and brush edits, in scene order (later roads win where they cross). Moving a
// point re-shapes only the land around the road: sample tiles, LOD meshes,
// colliders and the foliage / grass nearby (like a brush stroke).
//
// With followGround on (the default) the road's height is the ground under
// it smoothed along its length (`smoothing` m), so point heights don't
// matter; off, it follows the spline's own height (ramps, embankments).
// Vegetation: give the object a FoliageExclusion with `spline` on (the presets
// do). Navmeshes aren't rebuilt by themselves: call NavMeshSurface::rebuild.
// =============================================================================

#include "ve/render/Instancing.hpp"
#include "ve/scene/GameObject.hpp"
#include "ve/world/Terrain.hpp"

#include <memory>
#include <string>

namespace ve {

class TypeRegistry;

/// A road / path along the Spline on this object (see the top of this file).
/// The terrain fields are TerrainSplineShape's.
class TerrainSpline : public GameObject, public TerrainSplineShape {
public:
    TerrainSpline();
    ~TerrainSpline() override;
    void start() override;
    void update(float dt) override;
    void onDestroy() override;

    // ---- Surface mesh (optional) ----
    /// A material file for a mesh laid over the road (empty = none: the
    /// painted biome is the road). Its UVs: u across (0..1), v along.
    std::string material;
    float surfaceWidth = 0.0f;   ///< Mesh width (m; 0 = `width`), times each point's width factor.
    float uvLength = 4.0f;       ///< Metres of road per texture repeat along it.
    float surfaceLift = 0.04f;   ///< Mesh height above the ground (m): keeps it from flickering into the terrain.
    bool castShadows = false;    ///< The surface mesh casts shadows (usually not needed).

    /// Rebuild the surface (the inspector calls it after an edit).
    void markDirty() { m_dirty = true; }
    /// Something the Terrain compares to notice edits: the spline's version and the shape fields.
    std::string terrainState();
    /// The world-space area the road affects (x0, z0, x1, z1), or an empty one (x0 > x1).
    glm::vec4 bounds();

private:
    void buildSurface();
    std::shared_ptr<InstanceGroup> m_surface;
    uint64_t m_splineVersion = 0;
    uint64_t m_terrainVersion = 0;
    bool m_dirty = true;
};

/// Registers TerrainSpline with reflection (the Engine does at startup).
void registerTerrainSplineTypes(TypeRegistry& r);

} // namespace ve
