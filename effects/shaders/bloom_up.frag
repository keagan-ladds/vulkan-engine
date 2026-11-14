#version 460
// Bloom upsample: 3x3 tent filter, added onto the next larger mip (additive
// blending with alpha = weight).
#define VE_CUSTOM_PUSH_CONSTANTS
#include "ve.glsl"

layout(push_constant) uniform Push {
    vec2 texelSize; // of the source
    uint source;
    float weight;
    float radius;
} pc;

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

void main()
{
    vec2 o = pc.texelSize * pc.radius;
    vec3 c = veSampleLod(pc.source, inUV, 0.0).rgb * 4.0;
    c += (veSampleLod(pc.source, inUV + vec2(-o.x, 0), 0.0).rgb + veSampleLod(pc.source, inUV + vec2(o.x, 0), 0.0).rgb +
          veSampleLod(pc.source, inUV + vec2(0, -o.y), 0.0).rgb + veSampleLod(pc.source, inUV + vec2(0, o.y), 0.0).rgb) * 2.0;
    c += veSampleLod(pc.source, inUV + vec2(-o.x, -o.y), 0.0).rgb + veSampleLod(pc.source, inUV + vec2(o.x, -o.y), 0.0).rgb +
         veSampleLod(pc.source, inUV + vec2(-o.x, o.y), 0.0).rgb + veSampleLod(pc.source, inUV + vec2(o.x, o.y), 0.0).rgb;
    outColor = vec4(c / 16.0, pc.weight);
}
