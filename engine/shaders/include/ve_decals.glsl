// =============================================================================
// ve_decals.glsl - decals: bullet holes, puddles, cracks, dirt, paint...
//
// A decal is an oriented box (ve::DecalProjector, ve::DecalSystem) that
// projects its textures along its local -Z onto whatever surface lies inside
// it. This is a *forward* renderer, so decals aren't drawn on top of a lit
// image: material shaders change their surface with them *before* lighting,
//
//     VeSurface s = ...;   // the material's own albedo, normal, roughness, metallic
//     veApplyDecals(s, geometricNormal, dPdx, dPdy, receiveLayers);
//     outColor = vec4(veEvaluateLighting(s), 1.0);
//     outGBuffer = veGBuffer(s.normal, s.roughness);   // SSR sees wet puddles too
//
// so a decal can change any channel, each with its own opacity, and lights,
// shadows and reflections all see the changed surface. The decals of a frame
// are binned into the light cluster grid (ve::LightClusterer), so a pixel only
// tests the few whose boxes reach its cluster.
//
// Per decal (see ve::Decal for the C++ side):
//   albedo  : albedo texture rgb * colour; normal blend (mix) or multiply
//   normal  : a tangent-space normal map oriented on the decal (its +X = the
//             texture's u, +Y = up in the texture), built on the surface's
//             geometric normal and blended over the surface's own normal;
//             `flatten` first smooths out the surface's own bumps (water
//             filling a puddle)
//   rough / metal: values, multiplied by the mask texture's G / B
//   opacity : colour alpha * albedo texture alpha * mask alpha * fades,
//             times each channel's own opacity
//   fades   : surfaces turned away from the projection direction (angle fade,
//             never on back faces: no bleeding through thin walls), the
//             front/back of the box (depth fade), distance and lifetime (CPU)
// Overlapping decals are applied in order (sortOrder, then age): later on top.
// =============================================================================
#ifndef VE_DECALS_GLSL
#define VE_DECALS_GLSL

#include "ve_lighting.glsl" // (VeSurface; that file includes this one at its end)

#define VE_DECAL_LAYERS_ALL 0xFFFFFFFFu

// The decals that can cover a point (its cluster); off screen, or if the
// clusters overflowed, every decal.
struct VeDecalList {
    uint first, count;
    bool clustered;
};

VeDecalList veDecalList(vec3 P, float viewDepth)
{
    VeDecalList list = VeDecalList(0u, decals.info.x, false);
    if (decals.info.y == 0u || decals.info.x == 0u) return list;
    vec4 clip = frame.viewProj * vec4(P, 1.0);
    vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
    if (clip.w <= 0.0 || any(lessThan(uv, vec2(0.0))) || any(greaterThanEqual(uv, vec2(1.0)))) return list;
    ivec3 c = ivec3(int(uv.x * VE_CLUSTER_X), int(uv.y * VE_CLUSTER_Y),
                    clamp(int(floor(log(max(viewDepth, 1e-3)) * decals.clusterParams.x + decals.clusterParams.y)), 0, VE_CLUSTER_Z - 1));
    uint v = decals.clusters[(c.z * VE_CLUSTER_Y + c.y) * VE_CLUSTER_X + c.x];
    return VeDecalList(v >> 16, v & 0xFFFFu, true);
}

uint veDecalAt(VeDecalList list, uint k)
{
    if (!list.clustered) return k;
    uint i = list.first + k;
    return (decals.clusterDecals[i >> 1] >> (16u * (i & 1u))) & 0xFFFFu;
}

// Applies every decal covering s.position to the surface (call before lighting).
//   Ng            the geometric (interpolated vertex) normal, facing the viewer
//                 on double-sided back faces: decal normal maps are oriented on
//                 it, and the angle fade uses it
//   dPdx, dPdy    dFdx / dFdy of the world position, taken in uniform control
//                 flow (before any discard or branch): texture mip selection.
//                 Decal textures are sampled with explicit gradients, so this
//                 function itself may run in divergent control flow.
//   receiveLayers a decal applies where (its layers & receiveLayers) != 0
//                 (VE_DECAL_LAYERS_ALL: every decal)
void veApplyDecals(inout VeSurface s, vec3 Ng, vec3 dPdx, vec3 dPdy, uint receiveLayers)
{
    if (decals.info.x == 0u || receiveLayers == 0u) return;
    float viewDepth = dot(s.position - frame.cameraPosition.xyz, frame.cameraForward.xyz);
    VeDecalList list = veDecalList(s.position, viewDepth);
    vec4 P = vec4(s.position, 1.0);
    for (uint k = 0u; k < list.count; ++k) {
        uint i = veDecalAt(list, k);
        uvec4 tex = decals.list[i].textures;
        if ((tex.w & receiveLayers) == 0u) continue;
        vec4 r0 = decals.list[i].rows[0], r1 = decals.list[i].rows[1], r2 = decals.list[i].rows[2];
        vec3 local = vec3(dot(r0, P), dot(r1, P), dot(r2, P));
        if (any(greaterThan(abs(local), vec3(0.5)))) continue;

        VeDecal d = decals.list[i];
        // Angle fade: the box projects along its -Z, so surfaces facing its +Z
        // get the full decal; it fades out as they turn away (stretching).
        float facing = dot(Ng, normalize(r2.xyz));
        float a = d.color.a * smoothstep(d.fade.y, d.fade.x, facing);
        // Depth fade towards the front and back faces of the box.
        a *= 1.0 - smoothstep(1.0 - d.fade.z, 1.0, abs(local.z) * 2.0);
        if (a <= 0.0) continue;

        // Texture coordinates: u along the box's +X, v down its +Y (top-left origin).
        vec2 uv = vec2(local.x + 0.5, 0.5 - local.y);
        vec2 dx = vec2(dot(r0.xyz, dPdx), -dot(r1.xyz, dPdx));
        vec2 dy = vec2(dot(r0.xyz, dPdy), -dot(r1.xyz, dPdy));
        vec4 albedo = tex.x != VE_INVALID ? veSampleGrad(tex.x, uv, dx, dy) : vec4(1.0);
        vec4 mask = tex.z != VE_INVALID ? veSampleGrad(tex.z, uv, dx, dy) : vec4(1.0);
        a *= albedo.a * mask.a;
        if (a <= 0.002) continue;

        vec3 c = albedo.rgb * d.color.rgb;
        float wa = a * d.opacity.x;
        s.albedo = d.fade.w > 0.5 ? s.albedo * mix(vec3(1.0), c, wa) : mix(s.albedo, c, wa);
        s.roughness = mix(s.roughness, d.params.x * mask.g, a * d.opacity.z);
        s.metallic = mix(s.metallic, d.params.y * mask.b, a * d.opacity.w);

        vec3 N = s.normal;
        if (d.params.w > 0.0) N = normalize(mix(N, Ng, a * d.params.w)); // flatten the surface's bumps
        if (tex.y != VE_INVALID && d.opacity.y > 0.0) {
            vec3 tn = veUnpackNormal(veSampleGrad(tex.y, uv, dx, dy));
            tn.xy *= d.params.z;
            // Tangent frame: the box's +X (texture u) on the surface, and
            // N x T = the box's +Y (up in the texture) for a facing surface.
            vec3 X = normalize(r0.xyz);
            vec3 T = X - Ng * dot(Ng, X);
            T = dot(T, T) > 1e-8 ? normalize(T) : normalize(cross(abs(Ng.y) < 0.99 ? vec3(0, 1, 0) : vec3(1, 0, 0), Ng));
            vec3 B = cross(Ng, T);
            vec3 Nd = normalize(T * tn.x + B * tn.y + Ng * max(tn.z, 1e-3));
            N = normalize(mix(N, Nd, a * d.opacity.y));
        }
        s.normal = N;
    }
}

// Convenience: derivatives taken here (call in uniform control flow).
void veApplyDecals(inout VeSurface s, vec3 Ng, uint receiveLayers)
{
    veApplyDecals(s, Ng, dFdx(s.position), dFdy(s.position), receiveLayers);
}

#endif // VE_DECALS_GLSL
