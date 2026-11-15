#version 460
// Terrain vertex shader: world-space position/normal + biome vertex colour.
#include "ve.glsl"

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
    vec4 world = veModelMatrix() * vec4(inPosition, 1.0);
    outWorldPos = world.xyz;
    outNormal = normalize(mat3(veModelMatrix()) * inNormal);
    outColor = inColor;
    gl_Position = frame.viewProj * world;
}
