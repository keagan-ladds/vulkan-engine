#version 460
// Fire sheets: vertical quads turned around Y towards the camera (each sheet a
// little off-axis), bent downwind at the top; the horizontal coal bed; or the
// heat-haze sheet above the flames (fire_haze.frag).
#include "ve.glsl"

layout(location = 0) in vec4 inBaseHeight; // xyz base, w height
layout(location = 1) in vec4 inShape;      // radius, intensity, seed, kind (0 flame, 1 coals)
layout(location = 2) in vec4 inLean;       // xz tip lean (m), temperature, angle offset
layout(location = 3) in vec4 inAnim;       // time, sheet, sheets

layout(location = 0) out vec2 outUV;
layout(location = 1) flat out vec4 outShape;
layout(location = 2) flat out vec4 outParams; // temperature, time, sheets, unused
layout(location = 3) out float outViewDepth;

void main()
{
    const vec2 corners[6] = vec2[6](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));
    vec2 c = corners[gl_VertexIndex];
    vec3 base = inBaseHeight.xyz;
    float height = inBaseHeight.w, radius = inShape.x;
    vec3 world;
    if (inShape.w < 0.5) {
        vec3 toCam = frame.cameraPosition.xyz - base;
        toCam.y = 0.0;
        vec3 f = dot(toCam, toCam) > 1e-6 ? normalize(toCam) : vec3(0, 0, 1);
        float a = inLean.w, ca = cos(a), sa = sin(a);
        f = vec3(f.x * ca - f.z * sa, 0.0, f.x * sa + f.z * ca);
        vec3 right = vec3(f.z, 0.0, -f.x);
        float y01 = c.y * 0.5 + 0.5;
        // Room around the flame for its wobble: x in radii, y in heights.
        const float kWide = 1.6, kTall = 1.35;
        float depthOffset = (inAnim.y - (inAnim.z - 1.0) * 0.5) * radius * 0.18; // sheets a little apart: parallax
        world = base + right * (c.x * kWide * radius) + vec3(0.0, y01 * kTall * height, 0.0) +
                vec3(inLean.x, 0.0, inLean.y) * (y01 * y01) + f * depthOffset;
        outUV = vec2(c.x * kWide, y01 * kTall);
    } else if (inShape.w < 1.5) {
        world = base + vec3(c.x * radius, 0.03, c.y * radius);
        outUV = c;
    } else {
        // Heat haze: one camera-facing sheet over the flames, rising downwind.
        vec3 toCam = frame.cameraPosition.xyz - base;
        toCam.y = 0.0;
        vec3 f = dot(toCam, toCam) > 1e-6 ? normalize(toCam) : vec3(0, 0, 1);
        vec3 right = vec3(f.z, 0.0, -f.x);
        float y01 = c.y * 0.5 + 0.5;
        float halfWidth = radius * 2.2;
        world = base + right * (c.x * halfWidth) + vec3(0.0, mix(0.3, 3.0, y01) * height, 0.0) +
                vec3(inLean.x, 0.0, inLean.y) * (0.3 + y01 * 1.5) + f * radius * 0.3; // a little in front of the flames
        outUV = vec2(c.x, y01);
    }
    outShape = inShape;
    outParams = vec4(inLean.z, inAnim.x, inAnim.z, 0.0);
    vec4 view = frame.view * vec4(world, 1.0);
    outViewDepth = -view.z;
    gl_Position = frame.proj * view;
}
