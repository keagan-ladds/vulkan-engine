#version 460
// Upsamples the low-resolution FogBuffer and blends it over SceneColor
// (blend state: dst = src.rgb + dst * src.a).
#define VE_CUSTOM_PUSH_CONSTANTS
#include "ve.glsl"

layout(push_constant) uniform Push {
    uint fog;
} pc;

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

void main()
{
    // 4 bilinear taps, each averaging 2x2 low-res texels = a 4x4 box, which
    // matches the 4x4 Bayer jitter pattern used by fog.frag.
    vec2 texel = 2.0 * frame.screenSize.zw; // FogBuffer is half resolution
    vec4 c = veSampleLod(pc.fog, inUV + vec2(-texel.x, -texel.y), 0.0);
    c += veSampleLod(pc.fog, inUV + vec2(texel.x, -texel.y), 0.0);
    c += veSampleLod(pc.fog, inUV + vec2(-texel.x, texel.y), 0.0);
    c += veSampleLod(pc.fog, inUV + vec2(texel.x, texel.y), 0.0);
    outColor = c * 0.25;
}
