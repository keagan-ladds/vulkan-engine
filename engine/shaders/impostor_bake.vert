#version 460
// Impostor baking: renders a mesh with an orthographic frame camera.
#define VE_CUSTOM_PUSH_CONSTANTS
#include "ve.glsl"

layout(push_constant) uniform Push {
    mat4 viewProj;
    vec4 centreRadius;
    uint materialIndex;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 4) in vec4 inColor;

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec2 outUV;
layout(location = 2) out vec4 outColor;

void main()
{
    outNormal = inNormal;
    outUV = inUV;
    outColor = inColor;
    gl_Position = pc.viewProj * vec4(inPosition, 1.0);
}
