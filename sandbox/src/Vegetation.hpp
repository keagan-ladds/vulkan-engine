#pragma once

// =============================================================================
// Procedural low-poly props (acacia / baobab / fever trees, bushes, rocks,
// boulders), registered in ve::MeshLibrary so the scene file's Foliage layers
// can use them (FoliageType::library = "acacia"...). Where they grow, and the
// grass and reeds, are data in scenes/savanna.json.
// =============================================================================

#include <cstdint>

namespace savanna {

// Registers "acacia", "baobab", "fever tree", "bush", "rock" and "boulder"
// (LOD chains, impostors for trees, colliders, RaycastHit::userValue 1..6).
void registerPropMeshes();

// Display name for RaycastHit::userValue of a prop collider.
const char* propName(uint32_t userValue);

} // namespace savanna
