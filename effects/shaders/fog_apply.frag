#version 460
// Upsamples the low-resolution FogBuffer and blends it over SceneColor
// (blend state: dst = src.rgb + dst * src.a).
#define VE_CUSTOM_PUSH_CONSTANTS
#include "ve.glsl"

layout(push_constant) uniform Push {
    vec2 size; // FogBuffer size in texels
    uint fog;
} pc;

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

void main()
{
    // Exact 4x4 box over FogBuffer texels, matching fog.frag's 4x4 Bayer
    // jitter so the dither cancels: 4 bilinear taps on the shared corners of
    // 2x2 texel groups, each an exact 2x2 average (same scheme as ssgi_apply).
    vec2 p = inUV * pc.size - 0.5; // texel space, centres on integers
    vec2 k = floor(p + 1.0);       // window = texels k-2 .. k+1
    vec4 c = veSampleLod(pc.fog, (k + vec2(-1.0, -1.0)) / pc.size, 0.0);
    c += veSampleLod(pc.fog, (k + vec2(1.0, -1.0)) / pc.size, 0.0);
    c += veSampleLod(pc.fog, (k + vec2(-1.0, 1.0)) / pc.size, 0.0);
    c += veSampleLod(pc.fog, (k + vec2(1.0, 1.0)) / pc.size, 0.0);
    outColor = c * 0.25;
}
