// =============================================================================
// ve_terrain.glsl - ve::Terrain LOD geomorphing (vertex stage), shared by the
// colour and shadow vertex shaders so shadows match the ground exactly.
//
// Vertex layout written by TerrainLod: position/normal on this node's grid,
// tangent.x = height on the parent's (coarser) grid, tangent.yzw = the parent's
// normal there. Material params[3].xy = the camera distance range over which
// this level morphs into its parent (see TerrainLod::morphRange).
// =============================================================================
#ifndef VE_TERRAIN_GLSL
#define VE_TERRAIN_GLSL

#include "ve.glsl"

// 0 = this level's shape, 1 = the parent's (where the node hands over to it).
float veTerrainMorph(VeMaterial m, vec3 position)
{
    vec2 range = m.params[3].xy;
    return clamp((distance(position, frame.cameraPosition.xyz) - range.x) / max(range.y - range.x, 1e-3), 0.0, 1.0);
}

vec3 veTerrainPosition(VeMaterial m, vec3 position, vec4 tangent, out float morph)
{
    morph = veTerrainMorph(m, position);
    return vec3(position.x, mix(position.y, tangent.x, morph), position.z);
}

#endif // VE_TERRAIN_GLSL
