#version 460
// Terrain vertex shader: world-space position/normal + biome vertex colour and
// splat layers, geomorphed towards the parent LOD level with distance
// (ve_terrain.glsl). Vertex layout written by TerrainLod:
//   color.rgb = ground colour, color.a = the node's 3 splat layer indices (8 bits each)
//   uv.xy, tangent.w = those layers' weights; tangent.xyz = parent height, parent normal x/z
#include "ve_terrain.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inTangent;
layout(location = 4) in vec4 inColor;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec3 outColor;
layout(location = 3) out vec3 outSplat;
layout(location = 4) flat out uint outLayers;

void main()
{
    VeMaterial m = veMaterial();
    float morph;
    // Node vertices are in world space (the node renderers have identity transforms).
    vec4 world = veModelMatrix() * vec4(veTerrainPosition(m, inPosition, inTangent, morph), 1.0);
    vec3 parentNormal = vec3(inTangent.y, sqrt(max(1.0 - dot(inTangent.yz, inTangent.yz), 0.0)), inTangent.z);
    outWorldPos = world.xyz;
    outNormal = normalize(mat3(veModelMatrix()) * mix(inNormal, parentNormal, morph));
    outColor = inColor.rgb;
    outSplat = vec3(inUV, inTangent.w);
    outLayers = uint(inColor.a + 0.5);
    gl_Position = frame.viewProj * world;
}
