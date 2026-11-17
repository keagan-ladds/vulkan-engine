#version 460
// Motion vectors (ve::VelocityPass): where this surface was on screen last
// frame minus where it is now (unjittered UV), only where it is the visible
// surface (compared with SceneDepth; no depth attachment needed).
#define VE_CUSTOM_PUSH_CONSTANTS
#include "ve.glsl"

layout(location = 0) in vec4 inCurrent;
layout(location = 1) in vec4 inPrevious;

layout(location = 0) out vec2 outVelocity;

void main()
{
    // Reversed-Z: larger = closer. Hidden behind something else: keep the clear value.
    float scene = texelFetch(veTextures[nonuniformEXT(frame.builtinTextures.x)], ivec2(gl_FragCoord.xy), 0).r;
    if (gl_FragCoord.z < scene - 1e-5 * max(scene, 1e-3) * 50.0) discard;
    vec2 current = inCurrent.xy / inCurrent.w * 0.5 + 0.5 - frame.jitter.xy;
    vec2 previous = inPrevious.xy / inPrevious.w * 0.5 + 0.5;
    outVelocity = previous - current;
}
