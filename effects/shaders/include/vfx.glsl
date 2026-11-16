// =============================================================================
// vfx.glsl - shared definitions for the optional effects module.
//
// Global parameter convention (frame.globalParams[i]); set from C++ via
// renderer.setGlobalParam() (vfx::GlobalSlot in vfx/Vfx.hpp):
//   [0] wind:    xy = direction (world XZ, normalized), z = strength (m/s), w = gustiness
//   [1] weather: x = wetness 0..1, y = rain 0..1, z = cloud coverage 0..1, w = fog multiplier
//   [2] sun:     xyz = direction TOWARDS the sun, w = sun illuminance
//   [3] sunColor: rgb = sun colour at the ground (after atmosphere), w = unused
// =============================================================================
#ifndef VFX_GLSL
#define VFX_GLSL

#include "ve.glsl"

#define VFX_WIND frame.globalParams[0]
#define VFX_WEATHER frame.globalParams[1]
#define VFX_SUN frame.globalParams[2]
#define VFX_SUN_COLOR frame.globalParams[3]

// ---- Atmosphere (physically based single scattering, Earth-like) -------------
const float VFX_EARTH_RADIUS = 6360e3;
const float VFX_ATMOSPHERE_RADIUS = 6460e3;
const vec3 VFX_RAYLEIGH_SCATTER = vec3(5.802e-6, 13.558e-6, 33.1e-6);
const float VFX_RAYLEIGH_HEIGHT = 8000.0;
const float VFX_MIE_SCATTER = 3.996e-6;
const float VFX_MIE_EXTINCTION = 4.440e-6;
const float VFX_MIE_HEIGHT = 1200.0;
const vec3 VFX_OZONE_ABSORB = vec3(0.650e-6, 1.881e-6, 0.085e-6);

// Ray/sphere (centered at origin) intersection distances; x > y means miss.
vec2 vfxRaySphere(vec3 ro, vec3 rd, float radius)
{
    float b = dot(ro, rd);
    float c = dot(ro, ro) - radius * radius;
    float d = b * b - c;
    if (d < 0.0) return vec2(1e9, -1e9);
    d = sqrt(d);
    return vec2(-b - d, -b + d);
}

// Extinction coefficients at height h (meters above the ground).
vec3 vfxExtinction(float h, float mieScale)
{
    float rayleigh = exp(-h / VFX_RAYLEIGH_HEIGHT);
    float mie = exp(-h / VFX_MIE_HEIGHT) * mieScale;
    float ozone = max(0.0, 1.0 - abs(h - 25000.0) / 15000.0);
    return VFX_RAYLEIGH_SCATTER * rayleigh + VFX_MIE_EXTINCTION * mie + VFX_OZONE_ABSORB * ozone;
}

float vfxPhaseRayleigh(float c) { return 3.0 / (16.0 * VE_PI) * (1.0 + c * c); }

// Henyey-Greenstein phase: g > 0 scatters forward (bright halo around the sun).
float vfxPhaseHG(float c, float g)
{
    float g2 = g * g;
    float x = max(1.0 + g2 - 2.0 * g * c, 1e-4);
    return (1.0 - g2) / (4.0 * VE_PI * x * sqrt(x)); // x^1.5
}

// Mie phase used for the sky (Cornette-Shanks).
float vfxPhaseMie(float c, float g)
{
    float g2 = g * g;
    float k = 3.0 / (8.0 * VE_PI) * (1.0 - g2) / (2.0 + g2);
    float x = max(1.0 + g2 - 2.0 * g * c, 1e-4);
    return k * (1.0 + c * c) / (x * sqrt(x)); // x^1.5
}

// ---- Sky-view LUT lookup ----------------------------------------------------------
// The LUT stores sky radiance for every view direction: u = azimuth relative
// to the sun (0..pi), v = elevation with more resolution near the horizon.
vec2 vfxSkyLutUv(vec3 dir, vec3 sunDir)
{
    float elevation = asin(clamp(dir.y, -1.0, 1.0));
    float v = 0.5 + 0.5 * sign(elevation) * sqrt(abs(elevation) / (VE_PI * 0.5));
    vec2 d = dir.xz;
    vec2 s = sunDir.xz;
    float az = 0.0;
    if (dot(d, d) > 1e-8 && dot(s, s) > 1e-8) az = acos(clamp(dot(normalize(d), normalize(s)), -1.0, 1.0));
    return vec2(az / VE_PI, 1.0 - v);
}

vec3 vfxSampleSky(uint lutIndex, vec3 dir, vec3 sunDir)
{
    return veSampleLod(lutIndex, vfxSkyLutUv(dir, sunDir), 0.0).rgb;
}

// ---- Shadows for volumetrics (single tap, no PCF: noise averages it out) --------
// Sun shadow at p, and which cascade covered it (-1 = beyond the shadow
// distance or no shadowed sun; the result is then 1).
float vfxSunShadowCascade(vec3 p, out int cascade)
{
    cascade = -1;
    if (frame.lightCounts.x == 0u) return 1.0;
    VeDirectionalLight L = lights.directional[0];
    if (L.shadow.x < 0) return 1.0;
    float viewDepth = dot(p - frame.cameraPosition.xyz, frame.cameraForward.xyz);
    for (int i = 0; i < L.shadow.y; ++i) {
        if (viewDepth <= L.cascadeSplits[i]) { cascade = i; break; }
    }
    if (cascade < 0) return 1.0;
    vec4 sp = lights.shadowViews[L.shadow.x + cascade] * vec4(p, 1.0);
    vec3 ndc = sp.xyz / sp.w;
    return texture(veDirShadowMap, vec4(ndc.xy * 0.5 + 0.5, float(L.shadow.z + cascade), ndc.z));
}

int vfxSunCascadeCount()
{
    return frame.lightCounts.x == 0u || lights.directional[0].shadow.x < 0 ? 0 : lights.directional[0].shadow.y;
}

float vfxSunShadow(vec3 p)
{
    int cascade;
    return vfxSunShadowCascade(p, cascade);
}

float vfxRemap(float v, float lo, float hi, float newLo, float newHi)
{
    return newLo + (v - lo) / max(hi - lo, 1e-5) * (newHi - newLo);
}

#endif // VFX_GLSL
