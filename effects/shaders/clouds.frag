#version 460
// Volumetric clouds, rendered at reduced resolution for speed.
// Raymarches a flat slab of cloud between `bottom` and `top` altitude using a
// tiling 3D noise texture (R = Perlin-Worley base shape, GBA = Worley detail).
// Output: rgb = light scattered towards the camera, a = transmittance.
#define VE_CUSTOM_PUSH_CONSTANTS
#include "vfx.glsl"

layout(push_constant) uniform Push {
    vec4 layer;     // x = bottom, y = top, z = coverage 0..1, w = density
    vec4 shape;     // x = base scale (m), y = detail scale (m), z = detail strength, w = steps
    vec4 sunColor;  // rgb = sun radiance reaching the clouds, w = light steps
    vec4 ambient;   // rgb = sky ambient, w = forward scattering g
    vec2 wind;      // world XZ offset (m), animated on the CPU
    uint noiseTex;
    uint depthTex;
    uint skyLut;
} pc;

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

float cloudDensity(vec3 p, bool detail)
{
    float h = clamp((p.y - pc.layer.x) / (pc.layer.y - pc.layer.x), 0.0, 1.0);
    // Cumulus-like vertical profile: round bottoms, tapering tops.
    float profile = smoothstep(0.0, 0.12, h) * smoothstep(1.0, 0.45, h);
    // Outside the slab (light-march samples above the top) nothing can be there.
    if (profile <= 0.0) return 0.0;
    vec3 q = p + vec3(pc.wind.x, 0.0, pc.wind.y);
    // Large-scale coverage variation so the sky isn't uniformly cloudy.
    float weather = veSample3DLod(pc.noiseTex, vec3(q.xz / (pc.shape.x * 4.0), 0.37).xzy, 0.0).r;
    float coverage = clamp(pc.layer.z * (0.6 + 0.8 * weather), 0.0, 1.0);
    // The base noise is <= 1, so base <= 0 whenever profile <= 1 - coverage:
    // skip the shape fetch (exactly the same result).
    if (profile <= 1.0 - coverage) return 0.0;
    vec4 n = veSample3DLod(pc.noiseTex, q / pc.shape.x, 0.0);
    float base = vfxRemap(n.r * profile, 1.0 - coverage, 1.0, 0.0, 1.0);
    if (base <= 0.0 || !detail) return max(base, 0.0) * pc.layer.w;
    vec4 d = veSample3DLod(pc.noiseTex, (q + vec3(0.0, -frame.time.x * 4.0, 0.0)) / pc.shape.y, 0.0);
    float erode = (d.g * 0.625 + d.b * 0.25 + d.a * 0.125) * pc.shape.z;
    return clamp(vfxRemap(base, erode, 1.0, 0.0, 1.0), 0.0, 1.0) * pc.layer.w;
}

void main()
{
    float depth = veSampleLod(pc.depthTex, inUV, 0.0).r;
    vec3 dir = veViewRay(inUV);
    vec3 ro = frame.cameraPosition.xyz;
    // Only sky pixels (reversed-Z: depth 0 = nothing drawn), looking upwards.
    if (depth > 0.0 || dir.y < 0.01) {
        outColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    float t0 = (pc.layer.x - ro.y) / dir.y;
    float t1 = (pc.layer.y - ro.y) / dir.y;
    t0 = max(t0, 0.0);
    t1 = min(t1, t0 + 15000.0);
    if (t1 <= t0) {
        outColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    int steps = int(pc.shape.w);
    float stepLen = (t1 - t0) / float(steps);
    float jitter = veInterleavedGradientNoise(gl_FragCoord.xy + float(int(frame.time.z) % 64) * 5.588238);
    vec3 sunDir = VFX_SUN.xyz;
    float cosTheta = dot(dir, sunDir);
    // Dual-lobe phase: strong forward glow + some back scattering.
    float phase = mix(vfxPhaseHG(cosTheta, pc.ambient.w), vfxPhaseHG(cosTheta, -0.25), 0.3);
    int lightSteps = int(pc.sunColor.w);
    const float sigma = 0.035; // extinction per meter at density 1

    float ls = (pc.layer.y - pc.layer.x) * 0.5 / float(lightSteps); // light-march step

    vec3 scattered = vec3(0.0);
    float T = 1.0;
    for (int i = 0; i < steps; ++i) {
        vec3 p = ro + dir * (t0 + (float(i) + jitter) * stepLen);
        float dens = cloudDensity(p, true);
        if (dens <= 0.001) continue;

        // Light march towards the sun to see how much light reaches p.
        float lightOD = 0.0;
        for (int j = 1; j <= lightSteps; ++j) {
            lightOD += cloudDensity(p + sunDir * ls * float(j), false) * ls;
        }
        float lightT = exp(-lightOD * sigma);
        // "Powder" effect: darkens cloud edges facing the sun (Horizon Zero Dawn).
        float powder = 1.0 - exp(-dens * sigma * 2.0 * stepLen);
        float h = (p.y - pc.layer.x) / (pc.layer.y - pc.layer.x);
        vec3 light = pc.sunColor.rgb * lightT * phase * mix(1.0, powder * 2.0, 0.5) + pc.ambient.rgb * (0.4 + 0.6 * h);

        float ext = dens * sigma;
        float stepT = exp(-ext * stepLen);
        scattered += T * light * (1.0 - stepT); // energy-conserving integration (albedo ~1)
        T *= stepT;
        if (T < 0.01) break;
    }

    // Aerial perspective: distant clouds fade into the sky colour.
    float fade = exp(-t0 * 0.00006);
    if (T < 1.0) scattered = mix(vfxSampleSky(pc.skyLut, dir, sunDir) * (1.0 - T), scattered, fade);
    outColor = vec4(scattered, T);
}
