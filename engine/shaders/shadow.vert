#version 460
// Default shadow-map vertex shader (directional cascades AND point cubes).
//
// Contract for custom shadow vertex shaders (ShaderDesc::shadowVertex):
//   - position the vertex with lights.shadowViews[draw.viewIndex] * worldPos
//   - write the world position to location 0 (point shadows need it)
// Copy this file and add your own vertex animation (e.g. wind) in between.
#include "ve.glsl"

layout(location = 0) in vec3 inPosition;

layout(location = 0) out vec3 outWorldPos;

void main()
{
    vec4 world = draw.model * vec4(inPosition, 1.0);
    outWorldPos = world.xyz;
    gl_Position = lights.shadowViews[draw.viewIndex] * world;
}
