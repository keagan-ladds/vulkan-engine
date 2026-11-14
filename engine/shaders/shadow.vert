#version 460
// Default shadow-map vertex shader (directional cascades AND point cubes,
// regular AND instanced draws).
//
// Contract for custom shadow vertex shaders (ShaderDesc::shadowVertex):
//   - position the vertex with lights.shadowViews[draw.viewIndex] * worldPos
//   - write the world position to location 0 (point shadows need it)
//   - write the UV to location 1 (alpha-tested "cutout" shadows need it)
//   - use veModelMatrix() so instanced draws work
// Copy this file and add your own vertex animation (e.g. wind) in between.
#include "ve.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec2 outUV;

void main()
{
    vec4 world = veModelMatrix() * vec4(inPosition, 1.0);
    outWorldPos = world.xyz;
    outUV = inUV;
    gl_Position = lights.shadowViews[draw.viewIndex] * world;
}
