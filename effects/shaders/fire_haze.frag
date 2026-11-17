#version 460
// Heat haze over a fire: the scene behind (a copy of the screen around the
// fires, flames included) seen through rising, turbulent air. The offset is a
// couple of centimetres in the world, so it shrinks with distance; it fades
// at the sheet's edges and never pulls in things in front of the haze.
#define VE_CUSTOM_PUSH_CONSTANTS
#include "ve.glsl"

layout(push_constant) uniform Push {
    uint copyTexture;
    uint pad0, pad1, pad2;
    vec4 rect; // the copied uv rectangle
} pc;

layout(location = 0) in vec2 inUV;            // x -1..1 across, y 0..1 up
layout(location = 1) flat in vec4 inShape;    // radius, strength, seed, kind
layout(location = 2) flat in vec4 inParams;   // temperature, time, sheets
layout(location = 3) in float inViewDepth;

layout(location = 0) out vec4 outColor;

float fbm3(vec3 p)
{
    float s = 0.0, a = 0.5;
    for (int i = 0; i < 3; ++i) {
        s += a * veValueNoise(p);
        p = p * 2.07 + vec3(3.1, 7.7, 1.3);
        a *= 0.5;
    }
    return s / 0.875;
}

void main()
{
    float strength = inShape.y, seed = inShape.z, t = inParams.y;
    vec2 uv = inUV;
    // Strongest just above the flames, fading out up high and at the sides.
    float mask = (1.0 - smoothstep(0.35, 1.0, abs(uv.x))) * smoothstep(0.0, 0.2, uv.y) * (1.0 - smoothstep(0.45, 1.0, uv.y));
    if (mask <= 0.001) discard;

    vec2 screenUV = gl_FragCoord.xy * frame.screenSize.zw;
    float sceneHere = veLinearDepth(veSampleLod(frame.builtinTextures.x, screenUV, 0.0).r);
    if (sceneHere < inViewDepth - 0.05) discard; // something stands in front of the hot air

    // Turbulent air rising quickly: two decorrelated noises make the offset.
    vec3 p = vec3(uv.x * 2.5, uv.y * 4.0 - t * 2.6, seed + t * 0.6);
    vec2 n = vec2(fbm3(p), fbm3(p + vec3(17.3, 5.1, 9.7))) - 0.5;
    // ~5 cm of bending at full strength, as seen at this depth.
    vec2 metresToUV = vec2(abs(frame.proj[0][0]), abs(frame.proj[1][1])) * 0.5 / max(inViewDepth, 0.3);
    vec2 offset = n * 2.0 * 0.05 * strength * mask * metresToUV;

    vec2 src = clamp(screenUV + offset, pc.rect.xy, pc.rect.zw);
    // Don't pull in what's in front of the haze (a hand, a villager): no offset there.
    float sceneThere = veLinearDepth(veSampleLod(frame.builtinTextures.x, src, 0.0).r);
    if (sceneThere < inViewDepth - 0.05) src = screenUV;
    vec3 c = veSampleLod(pc.copyTexture, src, 0.0).rgb;
    outColor = vec4(c, mask);
}
