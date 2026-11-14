#version 460
// Bloom downsample (13-tap filter from Jimenez, "Next Generation Post
// Processing in Call of Duty: Advanced Warfare"). The first pass also applies
// a soft threshold and a Karis average to stop single bright pixels flickering.
#define VE_CUSTOM_PUSH_CONSTANTS
#include "ve.glsl"

layout(push_constant) uniform Push {
    vec2 texelSize; // of the source
    uint source;
    uint firstPass;
    float threshold;
    float knee;
} pc;

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

vec3 tap(vec2 offset) { return veSampleLod(pc.source, inUV + offset * pc.texelSize, 0.0).rgb; }

float karis(vec3 c) { return 1.0 / (1.0 + veLuminance(c)); }

void main()
{
    vec3 a = tap(vec2(-2, -2)), b = tap(vec2(0, -2)), c = tap(vec2(2, -2));
    vec3 d = tap(vec2(-1, -1)), e = tap(vec2(1, -1));
    vec3 f = tap(vec2(-2, 0)), g = tap(vec2(0, 0)), h = tap(vec2(2, 0));
    vec3 i = tap(vec2(-1, 1)), j = tap(vec2(1, 1));
    vec3 k = tap(vec2(-2, 2)), l = tap(vec2(0, 2)), m = tap(vec2(2, 2));

    vec3 color;
    if (pc.firstPass != 0u) {
        // Karis average per 2x2 block, then soft threshold.
        vec3 g0 = (d + e + i + j) * 0.25, g1 = (a + b + f + g) * 0.25, g2 = (b + c + g + h) * 0.25;
        vec3 g3 = (f + g + k + l) * 0.25, g4 = (g + h + l + m) * 0.25;
        float w0 = karis(g0) * 0.5, w1 = karis(g1) * 0.125, w2 = karis(g2) * 0.125;
        float w3 = karis(g3) * 0.125, w4 = karis(g4) * 0.125;
        color = (g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3 + g4 * w4) / (w0 + w1 + w2 + w3 + w4);
        float br = max(color.r, max(color.g, color.b));
        float soft = clamp(br - pc.threshold + pc.knee, 0.0, 2.0 * pc.knee);
        soft = soft * soft / (4.0 * pc.knee + 1e-4);
        color *= max(soft, br - pc.threshold) / max(br, 1e-4);
    } else {
        color = (d + e + i + j) * 0.125 + (a + c + k + m) * 0.03125 + (b + f + h + l) * 0.0625 + g * 0.125;
    }
    outColor = vec4(max(color, 0.0), 1.0);
}
