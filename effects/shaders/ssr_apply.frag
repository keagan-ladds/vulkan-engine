#version 460
// Upsamples the reduced-resolution SSR buffer (premultiplied) with a small
// blur and blends it over SceneColor (blend: dst = src.rgb + dst * (1 - src.a)).
#define VE_CUSTOM_PUSH_CONSTANTS
#include "ve.glsl"

layout(push_constant) uniform Push {
    vec2 texel; // SSR buffer texel size
    uint ssr;
} pc;

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

void main()
{
    vec2 o = pc.texel * 0.5;
    vec4 c = veSampleLod(pc.ssr, inUV + vec2(-o.x, -o.y), 0.0);
    c += veSampleLod(pc.ssr, inUV + vec2(o.x, -o.y), 0.0);
    c += veSampleLod(pc.ssr, inUV + vec2(-o.x, o.y), 0.0);
    c += veSampleLod(pc.ssr, inUV + vec2(o.x, o.y), 0.0);
    outColor = c * 0.25;
}
