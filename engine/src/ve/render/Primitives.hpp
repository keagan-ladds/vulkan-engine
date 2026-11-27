#pragma once

// =============================================================================
// Primitives - procedural MeshData generators. Combine them with
// MeshData::append() to build low-poly props (trees = cylinder + cones...).
// All are centred at the origin, +Y up, CCW front faces, with UVs.
// =============================================================================

#include "ve/render/Mesh.hpp"

namespace ve::primitives {

MeshData cube(const glm::vec3& size = glm::vec3(1.0f));
MeshData plane(float sizeX, float sizeZ, uint32_t segmentsX = 1, uint32_t segmentsZ = 1);
MeshData sphere(float radius, uint32_t segments = 24, uint32_t rings = 16);
/// Low-poly sphere made by subdividing an icosahedron (good for rocks).
MeshData icosphere(float radius, uint32_t subdivisions = 1);
/// Cylinder/cone along +Y from y=0 to y=height. radiusTop = 0 gives a cone.
MeshData cylinder(float radiusBottom, float radiusTop, float height, uint32_t segments = 12, bool caps = true);
/// Single quad facing +Z (useful for billboards / grass cards).
MeshData quad(float width, float height);

} // namespace ve::primitives
