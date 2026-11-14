#version 460
#define VE_CUSTOM_PUSH_CONSTANTS
#include "ve.glsl"

layout(push_constant) uniform Push {
    uint texture;
    uint flags;
    float stretch;
    float softness;
} pc;

layout(location = 0) in vec2 inUV;
layout(location = 1) in vec4 inColor;
layout(location = 2) in float inViewDepth;

layout(location = 0) out vec4 outColor;

void main()
{
    vec4 tex = veSample(pc.texture, inUV);
    vec4 c = tex * inColor;
    // Soft particles: fade out where the quad intersects scene geometry.
    vec2 screenUV = gl_FragCoord.xy * frame.screenSize.zw;
    float sceneDepth = veLinearDepth(veSampleLod(frame.builtinTextures.x, screenUV, 0.0).r);
    c.a *= clamp((sceneDepth - inViewDepth) / max(pc.softness, 1e-3), 0.0, 1.0);
    if (c.a <= 0.002) discard;
    outColor = c;
}
