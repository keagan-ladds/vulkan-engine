#version 460
// Sky: drawn only where no geometry was rendered (depth test against
// SceneDepth). Combines the sky-view LUT, the sun disc, stars at night and
// the (upsampled) volumetric clouds.
#define VE_CUSTOM_PUSH_CONSTANTS
#include "vfx.glsl"

layout(push_constant) uniform Push {
    vec4 sunDisk;    // rgb = sun disc radiance, w = cos(angular radius)
    vec4 moonDir;    // xyz = towards moon, w = moon disc brightness
    vec4 night;      // x = star brightness, y = night sky brightness
    uint skyLut;
    uint clouds;
} pc;

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

float stars(vec3 dir)
{
    // Hash a fine grid of directions; a few cells contain a star.
    vec3 p = dir * 300.0;
    vec3 cell = floor(p);
    float h = veHash13(cell);
    if (h < 0.996) return 0.0;
    vec3 center = cell + 0.5 + (vec3(veHash13(cell + 1.7), veHash13(cell + 3.1), veHash13(cell + 5.3)) - 0.5) * 0.6;
    float d = length(p - center);
    float twinkle = 0.7 + 0.3 * sin(frame.time.w * (2.0 + h * 40.0) + h * 100.0);
    return smoothstep(0.35, 0.0, d) * (h - 0.996) * 250.0 * twinkle;
}

void main()
{
    vec3 dir = veViewRay(inUV);
    vec3 sunDir = VFX_SUN.xyz;
    vec3 color = vfxSampleSky(pc.skyLut, dir, sunDir);

    // Night: a faint blue base + stars, fading in as the sun sets.
    float above = smoothstep(-0.05, 0.1, dir.y);
    color += vec3(0.010, 0.016, 0.035) * pc.night.y * (0.4 + 0.6 * above);
    color += vec3(stars(dir)) * pc.night.x * above;

    // Sun disc with soft limb.
    float c = dot(dir, sunDir);
    if (c > pc.sunDisk.w - 0.00002) {
        float edge = smoothstep(pc.sunDisk.w - 0.00002, pc.sunDisk.w + 0.00002, c);
        color += pc.sunDisk.rgb * edge;
    }
    // Moon disc.
    float m = dot(dir, pc.moonDir.xyz);
    color += vec3(0.8, 0.85, 1.0) * pc.moonDir.w * smoothstep(0.99985, 0.99992, m);

    // Clouds in front of everything above.
    vec4 cloud = veSampleLod(pc.clouds, inUV, 0.0);
    color = color * cloud.a + cloud.rgb;
    outColor = vec4(color, 1.0);
}
