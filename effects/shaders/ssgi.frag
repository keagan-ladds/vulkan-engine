#version 460
// Screen-space global illumination (one diffuse bounce) + ambient occlusion,
// traced at reduced resolution. A few cosine-distributed rays per pixel are
// marched a short distance through the depth buffer; rays that hit pick up
// that pixel's lit SceneColor as bounce light and count as occlusion.
// Output: rgb = bounce light to add, a = ambient occlusion (multiplier), for the
// InscatterTransmittance blend (dst = src.rgb + dst * src.a).
// The ray directions follow a 4x4 Bayer pattern; ssgi_apply.frag averages
// exactly 4x4 texels, which always contains all 16 rotations.
#define VE_CUSTOM_PUSH_CONSTANTS
#include "ve.glsl"

layout(push_constant) uniform Push {
    vec4 params; // x radius (m), y bounce intensity, z AO strength, w steps per ray
    uint depthTex;
    uint normalTex;
    uint colorTex;
    uint rays;
} pc;

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vec4(0.0, 0.0, 0.0, 1.0);
    float depth = veSampleLod(pc.depthTex, inUV, 0.0).r;
    vec4 g = veSampleLod(pc.normalTex, inUV, 0.0);
    if (depth <= 0.0 || g.w <= 0.0) return;

    vec3 P = veWorldFromDepth(inUV, depth);
    vec3 N = normalize(g.xyz);
    float pixelDepth = veLinearDepth(depth);
    float radius = pc.params.x;
    int steps = int(pc.params.w);

    // Tangent frame around N.
    vec3 T = normalize(abs(N.y) < 0.99 ? cross(N, vec3(0.0, 1.0, 0.0)) : cross(N, vec3(1.0, 0.0, 0.0)));
    vec3 B = cross(N, T);

    const float bayer[16] = float[16](0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);
    ivec2 bp = ivec2(gl_FragCoord.xy) & 3;
    // Both change every frame (golden-ratio sequence) so the temporal pass
    // (ssgi_temporal.frag) accumulates new samples instead of the same ones.
    float frameOffset = fract(frame.time.z * 0.6180340);
    float rotation = fract((bayer[bp.y * 4 + bp.x] + 0.5) / 16.0 + frameOffset);
    float stepJitter = fract(veInterleavedGradientNoise(gl_FragCoord.xy) + frameOffset * 1.618034);

    vec3 bounce = vec3(0.0);
    float occlusion = 0.0;
    uint rays = max(pc.rays, 1u);
    // Bias the start away from the surface (grows with distance: depth precision).
    vec3 origin = P + N * (0.03 + pixelDepth * 0.002);
    for (uint i = 0u; i < rays; ++i) {
        // Cosine-weighted hemisphere direction (golden-angle spiral, rotated per pixel).
        float u = (float(i) + 0.5) / float(rays);
        float phi = 6.2831853 * (float(i) * 0.618034 + rotation);
        float sinTheta = sqrt(u);
        vec3 dir = normalize(T * (cos(phi) * sinTheta) + B * (sin(phi) * sinTheta) + N * sqrt(1.0 - u));

        for (int s = 0; s < steps; ++s) {
            float f = (float(s) + stepJitter) / float(steps);
            float t = radius * f * f + 0.05;
            vec3 Q = origin + dir * t;
            vec4 clip = frame.viewProj * vec4(Q, 1.0);
            if (clip.w <= frame.cameraPosition.w) break;
            vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
            if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) break;
            float sd = veSampleLod(pc.depthTex, uv, 0.0).r;
            if (sd <= 0.0) continue; // sky
            float sceneDepth = veLinearDepth(sd);
            float diff = clip.w - sceneDepth;
            if (diff > 0.02 && diff < radius) {
                float falloff = 1.0 - f * f;
                occlusion += falloff;
                // Only light leaving the hit surface towards us bounces.
                vec3 hitN = veSampleLod(pc.normalTex, uv, 0.0).xyz;
                float facing = dot(hitN, hitN) > 0.0 ? clamp(dot(normalize(hitN), -dir) * 2.0, 0.0, 1.0) : 1.0;
                // Clamp very bright hits (small emissive objects) to tame fireflies.
                vec3 c = veSampleLod(pc.colorTex, uv, 0.0).rgb;
                c *= min(1.0, 4.0 / max(veLuminance(c), 1e-4));
                bounce += c * facing * falloff;
                break;
            }
        }
    }
    bounce /= float(rays);
    occlusion /= float(rays);

    // No albedo in the thin G-buffer: tint the bounce by the receiving
    // surface's own colour (its chroma) as an approximation.
    vec3 own = veSampleLod(pc.colorTex, inUV, 0.0).rgb;
    vec3 tint = own / max(max(own.r, max(own.g, own.b)), 1e-4) * 0.6;
    outColor = vec4(bounce * tint * pc.params.y, 1.0 - occlusion * pc.params.z);
}
