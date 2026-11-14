#version 460
#include "ve.glsl"

layout(location = 0) in vec3 inPosition;

layout(location = 0) out vec3 outWorldPos;

void main()
{
    vec4 world = draw.model * vec4(inPosition, 1.0);
    outWorldPos = world.xyz;
    gl_Position = frame.viewProj * world;
}
