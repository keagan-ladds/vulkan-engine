#version 460
// Sky-view LUT: integrates single scattering (Rayleigh + Mie + ozone) for a
// grid of view directions around the viewer. Small (192x108), so cheap even
// though every texel raymarches the atmosphere.
#define VE_CUSTOM_PUSH_CONSTANTS
#include "vfx.glsl"

layout(push_constant) uniform Push {
    vec4 sunDir;      // xyz towards sun, w = sun illuminance
    vec4 params;      // x = viewer height (m), y = mie scale (haze), z = mie g, w = multiple-scatter fudge
    vec4 groundAlbedo;
} pc;

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

vec3 transmittanceToSun(vec3 p, vec3 sunDir)
{
    // Earth's shadow: no direct sun below the horizon.
    vec2 ground = vfxRaySphere(p, sunDir, VFX_EARTH_RADIUS);
    if (ground.x > 0.0 && ground.x <= ground.y) return vec3(0.0);
    float len = vfxRaySphere(p, sunDir, VFX_ATMOSPHERE_RADIUS).y;
    const int N = 8;
    float ds = len / float(N);
    vec3 od = vec3(0.0);
    for (int i = 0; i < N; ++i) {
        vec3 q = p + sunDir * (float(i) + 0.5) * ds;
        od += vfxExtinction(length(q) - VFX_EARTH_RADIUS, pc.params.y) * ds;
    }
    return exp(-od);
}

void main()
{
    // Texel -> direction in a frame where the sun has azimuth 0.
    float v = 1.0 - inUV.y;
    float e = v * 2.0 - 1.0;
    float elevation = sign(e) * e * e * (VE_PI * 0.5);
    float azimuth = inUV.x * VE_PI;
    vec3 dir = vec3(cos(elevation) * cos(azimuth), sin(elevation), cos(elevation) * sin(azimuth));

    float sunElev = asin(clamp(pc.sunDir.y, -1.0, 1.0));
    vec3 sunDir = vec3(cos(sunElev), sin(sunElev), 0.0);

    vec3 ro = vec3(0.0, VFX_EARTH_RADIUS + max(pc.params.x, 1.0), 0.0);
    vec2 atmo = vfxRaySphere(ro, dir, VFX_ATMOSPHERE_RADIUS);
    vec2 ground = vfxRaySphere(ro, dir, VFX_EARTH_RADIUS);
    float tMax = atmo.y;
    bool hitGround = ground.x > 0.0 && ground.x <= ground.y;
    if (hitGround) tMax = ground.x;

    const int N = 30;
    float cosTheta = dot(dir, sunDir);
    float phaseR = vfxPhaseRayleigh(cosTheta);
    float phaseM = vfxPhaseMie(cosTheta, pc.params.z);

    vec3 radiance = vec3(0.0);
    vec3 throughput = vec3(1.0);
    float prevT = 0.0;
    for (int i = 0; i < N; ++i) {
        // Non-uniform steps: denser near the viewer where the air is thickest.
        float t = tMax * pow((float(i) + 0.5) / float(N), 2.0);
        float ds = t - prevT;
        prevT = t;
        vec3 p = ro + dir * t;
        float h = length(p) - VFX_EARTH_RADIUS;
        vec3 ext = vfxExtinction(h, pc.params.y);
        vec3 scatterR = VFX_RAYLEIGH_SCATTER * exp(-h / VFX_RAYLEIGH_HEIGHT);
        float scatterM = VFX_MIE_SCATTER * exp(-h / VFX_MIE_HEIGHT) * pc.params.y;
        vec3 sunT = transmittanceToSun(p, sunDir);
        // Crude multiple scattering: a little isotropic light wherever the
        // sun still reaches (so it fades out through twilight into night).
        float sunVis = dot(sunT, vec3(1.0 / 3.0));
        vec3 inscatter = sunT * (scatterR * phaseR + scatterM * phaseM) + (scatterR + scatterM) * pc.params.w * 0.08 * sunVis;
        vec3 stepT = exp(-ext * ds);
        radiance += throughput * inscatter * (1.0 - stepT) / max(ext, vec3(1e-9));
        throughput *= stepT;
    }
    if (hitGround) {
        // Light bounced off the ground below the horizon.
        vec3 p = ro + dir * tMax;
        vec3 n = normalize(p);
        radiance += throughput * transmittanceToSun(p, sunDir) * pc.groundAlbedo.rgb * max(dot(n, sunDir), 0.0) / VE_PI;
    }
    // Overcast: desaturate towards grey (groundAlbedo.w carries the amount).
    float lum = dot(radiance, vec3(0.2126, 0.7152, 0.0722));
    radiance = mix(radiance, vec3(lum), pc.groundAlbedo.w * 0.7);
    outColor = vec4(radiance * pc.sunDir.w, 1.0);
}
