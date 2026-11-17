#version 460
// Temporal anti-aliasing resolve (ve::TemporalAAPass). The camera is jittered
// by a different sub-pixel offset every frame; blending each frame into the
// history converges to a supersampled image.
//   - Reprojection: camera motion from the closest depth in the 3x3
//     neighbourhood (keeps edges of near objects from trailing).
//   - History: Catmull-Rom filtered (bilinear would blur a little every frame).
//   - Ghosting: the history is clipped to the variance box of the current
//     neighbourhood in YCoCg, so anything that moved or changed is rejected.
//   - Blending happens on tonemapped values so bright pixels don't flicker.
#define VE_CUSTOM_PUSH_CONSTANTS
#include "ve.glsl"

layout(push_constant) uniform Push {
    uint current;      // SceneColor (this frame, jittered)
    uint history;      // last frame's result
    uint depth;        // SceneDepth
    uint historyValid;
    float feedback;    // history weight
} pc;

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

vec3 toYCoCg(vec3 c) { return vec3(dot(c, vec3(0.25, 0.5, 0.25)), dot(c, vec3(0.5, 0.0, -0.5)), dot(c, vec3(-0.25, 0.5, -0.25))); }
vec3 fromYCoCg(vec3 c) { return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }
vec3 tonemap(vec3 c) { return c / (1.0 + max(c.r, max(c.g, c.b))); }
vec3 untonemap(vec3 c) { return c / max(1.0 - max(c.r, max(c.g, c.b)), 1e-4); }

vec3 historyAt(vec2 uv) { return textureLod(veTextures[nonuniformEXT(pc.history)], uv, 0.0).rgb; }

// Catmull-Rom with 5 bilinear taps (the corner taps are negligible).
vec3 historyCatmullRom(vec2 uv)
{
    vec2 size = frame.screenSize.xy, inv = frame.screenSize.zw;
    vec2 pos = uv * size;
    vec2 c = floor(pos - 0.5) + 0.5;
    vec2 f = pos - c;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 p0 = (c - 1.0) * inv, p3 = (c + 2.0) * inv, p12 = (c + w2 / w12) * inv;
    vec3 r = historyAt(vec2(p12.x, p0.y)) * w12.x * w0.y
           + historyAt(vec2(p0.x, p12.y)) * w0.x * w12.y
           + historyAt(p12) * w12.x * w12.y
           + historyAt(vec2(p3.x, p12.y)) * w3.x * w12.y
           + historyAt(vec2(p12.x, p3.y)) * w12.x * w3.y;
    float wsum = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(r / wsum, vec3(0.0));
}

// Moves `h` towards the box centre until it is inside the box.
vec3 clipToBox(vec3 lo, vec3 hi, vec3 h)
{
    vec3 centre = 0.5 * (hi + lo), extent = 0.5 * (hi - lo) + 1e-5;
    vec3 v = h - centre;
    vec3 a = abs(v / extent);
    float m = max(a.x, max(a.y, a.z));
    return m > 1.0 ? centre + v / m : h;
}

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 maxP = textureSize(veTextures[nonuniformEXT(pc.current)], 0) - 1;
    vec3 m1 = vec3(0.0), m2 = vec3(0.0), cur = vec3(0.0);
    float closest = -1.0;
    ivec2 closestP = p;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            ivec2 q = clamp(p + ivec2(x, y), ivec2(0), maxP);
            vec3 c = toYCoCg(tonemap(max(texelFetch(veTextures[nonuniformEXT(pc.current)], q, 0).rgb, vec3(0.0))));
            m1 += c;
            m2 += c * c;
            if (x == 0 && y == 0) cur = c;
            float d = texelFetch(veTextures[nonuniformEXT(pc.depth)], q, 0).r; // reversed-Z: larger = closer
            if (d > closest) {
                closest = d;
                closestP = q;
            }
        }
    }
    if (pc.historyValid == 0u) {
        outColor = vec4(untonemap(fromYCoCg(cur)), 1.0);
        return;
    }

    // Where this pixel's surface was last frame (both without jitter).
    vec2 uvC = (vec2(closestP) + 0.5) * frame.screenSize.zw;
    vec4 prev = frame.prevViewProj * vec4(veWorldFromDepth(uvC, closest), 1.0);
    vec2 motion = prev.xy / prev.w * 0.5 + 0.5 - (uvC - frame.jitter.xy);
    vec2 huv = inUV + motion;
    if (prev.w <= 0.0 || any(lessThan(huv, vec2(0.0))) || any(greaterThan(huv, vec2(1.0)))) {
        outColor = vec4(untonemap(fromYCoCg(cur)), 1.0);
        return;
    }

    vec3 mean = m1 / 9.0, sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));
    vec3 hist = clipToBox(mean - 1.25 * sigma, mean + 1.25 * sigma, toYCoCg(tonemap(historyCatmullRom(huv))));
    // Fast motion: rely a little more on the current frame (less smearing).
    float speed = length(motion * frame.screenSize.xy);
    float feedback = pc.feedback * mix(1.0, 0.85, clamp(speed * 0.125, 0.0, 1.0));
    outColor = vec4(untonemap(fromYCoCg(mix(cur, hist, feedback))), 1.0);
}
