#version 460
// Screen-space reflections, traced at reduced resolution. For each pixel with
// a smooth enough surface (SceneNormal.w = roughness) the reflected view ray
// is marched through the depth buffer; the hit pixel's SceneColor is the
// reflection. Output is premultiplied: rgb = colour * weight, a = weight.
#define VE_CUSTOM_PUSH_CONSTANTS
#include "ve.glsl"

layout(push_constant) uniform Push {
    vec4 params; // x max distance (m), y thickness (m), z steps, w max roughness
    vec4 params2; // x intensity, y edge fade, zw unused
    uint depthTex;
    uint normalTex;
    uint colorTex;
} pc;

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

vec3 viewPos(vec2 uv, float depth)
{
    vec4 v = frame.invProj * vec4(uv * 2.0 - 1.0, depth, 1.0);
    return v.xyz / v.w;
}

vec2 projectUV(vec3 viewPos)
{
    vec4 c = frame.proj * vec4(viewPos, 1.0);
    return c.xy / c.w * 0.5 + 0.5;
}

// Positive distance in front of the camera of the surface seen at uv.
float sceneViewDepth(vec2 uv)
{
    float d = veSampleLod(pc.depthTex, uv, 0.0).r;
    return d <= 0.0 ? 1e9 : veLinearDepth(d);
}

void main()
{
    outColor = vec4(0.0);
    float depth = veSampleLod(pc.depthTex, inUV, 0.0).r;
    vec4 g = veSampleLod(pc.normalTex, inUV, 0.0);
    float roughness = g.w;
    if (depth <= 0.0 || roughness <= 0.0 || roughness > pc.params.w) return;

    vec3 P = viewPos(inUV, depth);
    vec3 N = normalize(mat3(frame.view) * g.xyz);
    vec3 V = normalize(P);
    vec3 R = reflect(V, N);
    float NdotV = max(dot(-V, N), 0.0);

    float maxDist = pc.params.x;
    float thickness = pc.params.y;
    int steps = int(pc.params.z);
    // Varies every frame so the temporal accumulator averages it away.
    float jitter = fract(veInterleavedGradientNoise(gl_FragCoord.xy) + frame.time.z * 0.6180340);

    // Start slightly off the surface to avoid self-hits.
    vec3 origin = P + N * (0.02 + 0.002 * -P.z);
    float prevT = 0.0;
    float hitT = -1.0;
    vec2 hitUV = vec2(0.0);
    for (int i = 0; i < steps; ++i) {
        // Quadratic step distribution: dense near the surface, sparse far away.
        float f = (float(i) + jitter) / float(steps);
        float t = maxDist * f * f;
        vec3 Q = origin + R * t;
        if (Q.z > -frame.cameraPosition.w) break; // went behind the camera
        vec2 uv = projectUV(Q);
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) break;
        float rayDepth = -Q.z;
        float diff = rayDepth - sceneViewDepth(uv);
        if (diff > 0.0 && diff < thickness * (1.0 + t * 0.1)) {
            // Binary refinement between the last miss and this hit.
            float lo = prevT, hi = t;
            for (int k = 0; k < 5; ++k) {
                float mid = 0.5 * (lo + hi);
                vec3 M = origin + R * mid;
                vec2 muv = projectUV(M);
                if (-M.z - sceneViewDepth(muv) > 0.0) hi = mid; else lo = mid;
            }
            hitT = hi;
            hitUV = projectUV(origin + R * hi);
            break;
        }
        prevT = t;
    }
    if (hitT < 0.0) return;

    // Confidence: fade at the screen edges, with distance and with roughness;
    // reflections of back faces (ray hitting a surface facing away) are dropped.
    vec2 edge = smoothstep(vec2(0.0), vec2(pc.params2.y), hitUV) * smoothstep(vec2(0.0), vec2(pc.params2.y), 1.0 - hitUV);
    float conf = edge.x * edge.y;
    conf *= 1.0 - smoothstep(0.6, 1.0, hitT / maxDist);
    // Fade out over the upper half of the roughness range (no blur for glossy
    // reflections; rough-ish surfaces just get weaker ones).
    conf *= 1.0 - smoothstep(0.5, 1.0, roughness / pc.params.w);
    vec3 hitN = veSampleLod(pc.normalTex, hitUV, 0.0).xyz;
    if (dot(hitN, hitN) > 0.0) conf *= smoothstep(-0.05, 0.2, dot(normalize(mat3(frame.view) * hitN), -R));

    float fresnel = 0.04 + 0.96 * pow(1.0 - NdotV, 5.0);
    float weight = clamp(conf * fresnel * pc.params2.x, 0.0, 1.0);
    vec3 c = veSampleLod(pc.colorTex, hitUV, 0.0).rgb;
    c = min(c, vec3(64.0)); // tame fireflies (sun glints)
    outColor = vec4(c * weight, weight);
}
