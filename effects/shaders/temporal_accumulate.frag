#version 460
// Temporal accumulation of a noisy screen-space buffer (vfx::TemporalAccumulator,
// used by SSGI and SSR; any resolution). Reprojects last frame's
// accumulated result with frame.prevViewProj, rejects it where the surface
// changed (stored linear depth differs) or it left the screen, clips it to the
// current 3x3 neighbourhood's mean +- gamma*sigma (limits ghosting) and blends
// in this frame's trace. Outputs: 0 = accumulated (same layout as the input),
// 1 = linear depth (for next frame's rejection).
#define VE_CUSTOM_PUSH_CONSTANTS
#include "ve.glsl"

layout(push_constant) uniform Push {
    float blend;        // weight of the new frame (0.1 = ~10 frames of history)
    float clipGamma;
    uint historyValid;
    uint current;       // this frame's trace
    uint history;       // last frame's accumulated
    uint historyDepth;  // last frame's linear depth
    uint depthTex;      // SceneDepth
} pc;

layout(location = 0) in vec2 inUV;

vec4 cur0() { return texelFetch(veTextures[nonuniformEXT(pc.current)], ivec2(gl_FragCoord.xy), 0); }
layout(location = 0) out vec4 outAccum;
layout(location = 1) out float outDepth;

void main()
{
    float depth = veSampleLod(pc.depthTex, inUV, 0.0).r;
    ivec2 texel = ivec2(gl_FragCoord.xy);
    if (depth <= 0.0) {
        outAccum = cur0();
        outDepth = 0.0;
        return;
    }
    float lin = veLinearDepth(depth);

    // Neighbourhood statistics of the current (noisy) trace.
    vec4 m1 = vec4(0.0), m2 = vec4(0.0);
    ivec2 maxTexel = textureSize(veTextures[nonuniformEXT(pc.current)], 0) - 1;
    vec4 cur = vec4(0.0);
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec4 c = texelFetch(veTextures[nonuniformEXT(pc.current)], clamp(texel + ivec2(x, y), ivec2(0), maxTexel), 0);
            m1 += c;
            m2 += c * c;
            if (x == 0 && y == 0) cur = c;
        }
    }
    vec4 mean = m1 / 9.0;
    vec4 sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));

    vec4 result = mean; // fresh pixels: start from the local mean (less noisy than one texel)
    if (pc.historyValid != 0u) {
        vec4 prev = frame.prevViewProj * vec4(veWorldFromDepth(inUV, depth), 1.0);
        vec2 prevUV = prev.xy / prev.w * 0.5 + 0.5;
        if (prev.w > 0.0 && all(greaterThanEqual(prevUV, vec2(0.0))) && all(lessThanEqual(prevUV, vec2(1.0)))) {
            float hd = veSampleLod(pc.historyDepth, prevUV, 0.0).r;
            if (hd > 0.0 && abs(hd - prev.w) < 0.05 * prev.w + 0.05) {
                vec4 hist = veSampleLod(pc.history, prevUV, 0.0);
                hist = clamp(hist, mean - pc.clipGamma * sigma, mean + pc.clipGamma * sigma);
                result = mix(hist, cur, pc.blend);
            }
        }
    }
    outAccum = result;
    outDepth = lin;
}
