#version 460
// Terrain shadow vertex shader: the default shadow.vert contract with the
// same geomorph as terrain.vert, so the ground shadows itself exactly.
#include "ve_terrain.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inTangent;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec2 outUV;

void main()
{
    float morph;
    vec4 world = veModelMatrix() * vec4(veTerrainPosition(veMaterial(), inPosition, inTangent, morph), 1.0);
    outWorldPos = world.xyz;
    outUV = inUV;
    gl_Position = lights.shadowViews[draw.viewIndex] * world;
}
