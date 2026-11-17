#version 460
// Upsamples the accumulated SSGI (see temporal_accumulate.frag) and applies it (blend: dst = src.rgb + dst * src.a).
// ssgi.frag rotates its rays by a 4x4 Bayer pattern over the half-res pixels,
// so ANY 4x4 window of them holds all 16 rotations: averaging exactly 4x4
// texels removes the pattern. Texels at a different depth are down-weighted
// (bilateral) so bounce light and AO don't bleed across silhouettes.
#define VE_CUSTOM_PUSH_CONSTANTS
#include "ve.glsl"

layout(push_constant) uniform Push {
    vec2 size;  // SSGIBuffer size in texels
    uint ssgi;
    uint depthTex;
    uint normalTex;
} pc;

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

void main()
{
    float depth = veSampleLod(pc.depthTex, inUV, 0.0).r;
    if (depth <= 0.0) {
        outColor = vec4(0.0, 0.0, 0.0, 1.0); // sky: untouched
        return;
    }
    float d0 = veLinearDepth(depth);
    vec3 n0 = veSampleLod(pc.normalTex, inUV, 0.0).xyz;
    bool hasN0 = dot(n0, n0) > 0.0;
    if (hasN0) n0 = normalize(n0);

    // 4x4 texel window as 4 bilinear taps on the shared corners of 2x2 groups:
    // each tap is an exact 2x2 average, the four together an exact 4x4.
    vec2 p = inUV * pc.size - 0.5;   // texel space, centres on integers
    vec2 k = floor(p + 1.0);         // window = texels k-2 .. k+1
    vec4 sum = vec4(0.0);
    float wsum = 0.0;
    for (int i = 0; i < 4; ++i) {
        vec2 corner = k - 1.0 + 2.0 * vec2(i & 1, i >> 1); // texel-space corner (k-1 or k+1)
        vec2 uv = corner / pc.size;
        float d = veSampleLod(pc.depthTex, uv, 0.0).r;
        if (d <= 0.0) continue;
        // Edge-aware: same surface orientation and roughly the same depth
        // (relative tolerance: oblique ground changes depth quickly).
        vec3 n = veSampleLod(pc.normalTex, uv, 0.0).xyz;
        float c = hasN0 && dot(n, n) > 0.0 ? max(dot(n0, normalize(n)), 0.0) : 1.0;
        c *= c;
        float wn = c * c; // cos^4
        float wd = 1.0 - smoothstep(0.1, 0.3, abs(veLinearDepth(d) - d0) / d0);
        float w = wn * wd + 1e-3;
        sum += veSampleLod(pc.ssgi, uv, 0.0) * w;
        wsum += w;
    }
    outColor = wsum > 0.0 ? sum / wsum : vec4(0.0, 0.0, 0.0, 1.0);
}
