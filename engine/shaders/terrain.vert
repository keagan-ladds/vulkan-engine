#version 460
// Terrain vertex shader: world-space position/normal + biome vertex colour,
// geomorphed towards the parent LOD level with distance (ve_terrain.glsl).
#include "ve_terrain.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inTangent;
layout(location = 4) in vec4 inColor;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec4 outColor;

void main()
{
    VeMaterial m = veMaterial();
    float morph;
    // Node vertices are in world space (the node renderers have identity transforms).
    vec4 world = veModelMatrix() * vec4(veTerrainPosition(m, inPosition, inTangent, morph), 1.0);
    outWorldPos = world.xyz;
    outNormal = normalize(mat3(veModelMatrix()) * mix(inNormal, inTangent.yzw, morph));
    outColor = inColor;
    gl_Position = frame.viewProj * world;
}
