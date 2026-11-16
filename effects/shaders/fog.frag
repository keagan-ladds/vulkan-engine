#version 460
// Volumetric height fog with sun shafts, rendered at reduced resolution into
// FogBuffer (rgb = in-scattered light, a = transmittance) and then blended
// over SceneColor by fog_apply.frag.
//
// Raymarches from the camera to the scene depth, accumulating light from the
// sun (with shadows -> god rays), the sky (ambient) and nearby point lights.
#define VE_CUSTOM_PUSH_CONSTANTS
#include "vfx.glsl"

layout(push_constant) uniform Push {
    vec4 density;   // x = density at base height, y = height falloff, z = base height, w = noise amount
    vec4 scatter;   // x = anisotropy g, y = max distance (sky), z = steps, w = ambient strength
    vec4 sunColor;  // rgb sun radiance, w = sun shaft strength
    vec4 tint;      // rgb fog albedo tint, w = point light strength
    vec2 wind;      // noise offset (m)
    uint depthTex;
    uint skyLut;
    uint noiseTex;  // 3D noise (VE_INVALID = procedural fallback)
} pc;

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

float fogDensity(vec3 p)
{
    float h = p.y - pc.density.z;
    float d = pc.density.x * exp(-max(h, 0.0) * pc.density.y);
    // Slow rolling noise makes the mist patchy (great in the swamp).
    vec3 q = (p + vec3(pc.wind.x, frame.time.x * 0.3, pc.wind.y)) * 0.012;
    float n = pc.noiseTex != VE_INVALID ? veSample3DLod(pc.noiseTex, q, 0.0).r : veValueNoise(q * 4.0);
    return d * mix(1.0, n * 2.0, pc.density.w) * VFX_WEATHER.w;
}

void main()
{
    float depth = veSampleLod(pc.depthTex, inUV, 0.0).r;
    vec3 ro = frame.cameraPosition.xyz;
    vec3 target = veWorldFromDepth(inUV, max(depth, 1e-7));
    vec3 rd = normalize(target - ro);
    float dist = depth > 0.0 ? length(target - ro) : pc.scatter.y;
    dist = min(dist, pc.scatter.y);

    // Only point lights whose sphere the ray actually crosses are evaluated.
    uint nearLights[4];
    uint nearCount = 0u;
    for (uint li = 0u; li < min(frame.lightCounts.y, 16u) && nearCount < 4u; ++li) {
        vec4 pr = lights.point[li].positionRange;
        vec3 oc = ro - pr.xyz;
        float b = dot(oc, rd);
        float c = dot(oc, oc) - pr.w * pr.w;
        float disc = b * b - c;
        if (disc > 0.0 && -b + sqrt(disc) > 0.0 && -b - sqrt(disc) < dist) nearLights[nearCount++] = li;
    }

    int steps = int(pc.scatter.z);
    // Ordered 4x4 Bayer jitter: every 4x4 block covers all 16 offsets, and
    // fog_apply's blur averages exactly such a block, so the dither vanishes.
    const float bayer[16] = float[16](0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);
    ivec2 bp = ivec2(gl_FragCoord.xy) & 3;
    float jitter = (bayer[bp.y * 4 + bp.x] + 0.5) / 16.0;
    vec3 sunDir = VFX_SUN.xyz;
    float phase = vfxPhaseHG(dot(rd, sunDir), pc.scatter.x);
    vec3 sunLight = pc.sunColor.rgb * phase * pc.sunColor.w;
    // Sky light reaching the fog from all around (not the sky behind it in
    // the view direction, which would make fog glow orange towards a sunset
    // even when the sun is hidden behind terrain).
    vec3 ambient = vfxSampleSky(pc.skyLut, vec3(0.0, 1.0, 0.0), sunDir) * 0.4;
    for (int k = 0; k < 6; ++k) {
        float a = float(k) * 1.0471976;
        ambient += vfxSampleSky(pc.skyLut, normalize(vec3(cos(a), 0.35, sin(a))), sunDir) * 0.1;
    }
    ambient *= pc.scatter.w;

    // Beyond the shadow cascades there's no shadow information. Treating that
    // fog as sunlit makes it glow after the sun has gone behind distant
    // terrain, so instead carry on with the average sun visibility seen in
    // the last cascade along this ray (or the camera's own when the ray
    // never got that far).
    int lastCascade = vfxSunCascadeCount() - 1;
    float farVisSum = 0.0, farVisWeight = 0.0;
    float nearVis = vfxSunShadow(ro + sunDir * 0.5);

    // Sun shadow along the ray without a cascade search and a mat4 per step:
    // view depth grows linearly with t, so the cascade index only ever moves
    // forward, and the (orthographic) shadow projection of ro + rd * t is
    // sOrigin + sDir * t. Both are only recomputed when the cascade changes.
    // Same result as vfxSunShadowCascade(p).
    float depthPerT = dot(rd, frame.cameraForward.xyz);
    int cascade = 0;
    int cascadeReady = -1; // cascade sOrigin/sDir belong to
    vec3 sOrigin = vec3(0.0), sDir = vec3(0.0);

    vec3 scattered = vec3(0.0);
    float T = 1.0;
    float prevT = 0.0;
    for (int i = 0; i < steps; ++i) {
        // Quadratic sample distribution: more samples near the camera.
        float f = (float(i) + jitter) / float(steps);
        float t = dist * f * f;
        float ds = t - prevT;
        prevT = t;
        vec3 p = ro + rd * t;
        float sigma = fogDensity(p);
        if (sigma <= 1e-6) continue;

        float vis = 1.0;
        if (lastCascade >= 0) {
            VeDirectionalLight SL = lights.directional[0];
            float viewDepth = t * depthPerT;
            while (cascade <= lastCascade && viewDepth > SL.cascadeSplits[cascade]) ++cascade;
            if (cascade <= lastCascade) {
                if (cascadeReady != cascade) {
                    mat4 M = lights.shadowViews[SL.shadow.x + cascade];
                    sOrigin = (M * vec4(ro, 1.0)).xyz;
                    sDir = mat3(M) * rd;
                    cascadeReady = cascade;
                }
                vec3 ndc = sOrigin + sDir * t;
                vis = texture(veDirShadowMap, vec4(ndc.xy * 0.5 + 0.5, float(SL.shadow.z + cascade), ndc.z));
            }
        }
        if (cascade > lastCascade && lastCascade >= 0) {
            vis = farVisWeight > 0.0 ? farVisSum / farVisWeight : nearVis;
        } else if (cascade == lastCascade) {
            farVisSum += vis * ds;
            farVisWeight += ds;
        }
        vec3 light = ambient + sunLight * vis;
        for (uint k = 0u; k < nearCount; ++k) {
            VePointLight L = lights.point[nearLights[k]];
            float d = length(L.positionRange.xyz - p);
            float r = d / L.positionRange.w;
            if (r < 1.0) light += L.color.rgb * ((1.0 - r * r) * (1.0 - r * r) / (d * d + 1.0)) * pc.tint.w / (4.0 * VE_PI);
        }
        light *= pc.tint.rgb;
        float stepT = exp(-sigma * ds);
        scattered += T * light * (1.0 - stepT);
        T *= stepT;
        if (T < 0.003) break;
    }
    outColor = vec4(scattered, T);
}
