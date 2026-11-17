#version 460
// Motion vectors (ve::VelocityPass): a moving object's mesh with this and
// last frame's world matrix. Rasterised with the (jittered) camera so it
// lines up with the scene; the motion itself uses unjittered positions.
#define VE_CUSTOM_PUSH_CONSTANTS
#include "ve.glsl"

layout(push_constant) uniform Push {
    mat4 model;
    mat4 prevModel;
} pc;

layout(location = 0) in vec3 inPosition;

layout(location = 0) out vec4 outCurrent;  // clip position (jittered)
layout(location = 1) out vec4 outPrevious; // last frame's clip position (unjittered)

void main()
{
    vec4 world = pc.model * vec4(inPosition, 1.0);
    outCurrent = frame.viewProj * world;
    outPrevious = frame.prevViewProj * (pc.prevModel * vec4(inPosition, 1.0));
    gl_Position = outCurrent;
}
