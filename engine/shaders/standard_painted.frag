#version 460
// =============================================================================
// standard_painted.frag - standard.frag + vertex-painted dirt, wear and
// wetness. A starting point / placeholder: copy and tweak it for your game.
//
// Use it from a material file (assets/MaterialAsset.hpp) with standard.vert:
//
//   {
//     "version": 1,
//     "fragmentShader": "standard_painted.frag",
//     "albedo": "textures/plaster.png",
//     "textures": [
//       {"slot": 5, "path": "textures/dirt.png"},      // optional
//       {"slot": 6, "path": "textures/grunge.png"},   // optional (R)
//       {"slot": 7, "path": "textures/brick.png"}      // optional
//     ],
//     "params": [
//       {"slot": 4, "value": [1, 1, 1, 0.8]},           // strengths: dirt, wear, wet, grunge breakup
//       {"slot": 5, "value": [0.18, 0.14, 0.1, 0.95]},  // dirt colour, dirt roughness
//       {"slot": 6, "value": [0.3, 0.2, 0.13, 0.85]},   // wear colour (linear), wear roughness
//       {"slot": 7, "value": [0.5, 3, 0.65, 0.2]}       // world scale, edge sharpness, wet darkening, wet roughness (0.05 = puddle-glossy)
//     ]
//   }
//
// Blender preview: tools/blender/ve_painted_node_group.py builds a node group
// with the same maths (keep it in step: tools/blender/test_ve_painted.py).
//
// Vertex colour = the paint (Blender: Color Attribute, Vertex Paint mode):
//   R = dirt    (dirt colour / texture over the surface, rougher, a little AO)
//   G = wear    (the "under" material showing through: chipped plaster, bare wood)
//   B = wetness (darker, smoother, bumps flattened: SSR reflects it)
//   A = unused
// 0 = clean, 1 = full. FILL THE COLOUR ATTRIBUTE WITH BLACK BEFORE PAINTING:
// a mesh without vertex colours reads as white (fully painted). Or flip a
// channel with params[8].xyz = 1 (paints with "white = clean").
//
// Parameter layout (params 0-3 and textures 0-4 = StandardMaterial's, see
// ve::StandardMaterial; params[3].z (vertex colour tint) is ignored here):
//   params[4]   = strengths: x dirt, y wear, z wet, w grunge breakup (0..1)
//   params[5]   = dirt colour rgb (when textures[5] is unset; else tints it), w = dirt roughness
//                 (all zero: dark brown, 0.95)
//   params[6]   = wear colour rgb (when textures[7] is unset; else tints it), w = wear roughness
//                 (all zero: bare clay, 0.85). Colours are linear: darker than they look in sRGB pickers
//   params[7]   = x overlay scale (repeats per metre, world space; 0 = 0.5),
//                 y edge sharpness (>= 1; 0 = 3), z wet darkening (0..1), w wet roughness
//   params[8]   = xyz invert dirt / wear / wet mask (1 = white is clean), w unused
//   textures[5] = dirt albedo (sRGB)          triplanar in world space, so it
//   textures[6] = grunge / breakup mask (R)   doesn't depend on the mesh's UVs
//   textures[7] = wear albedo (sRGB)          and continues across modular pieces
// All-zero params 4-8 (a material without them) = plain standard.frag.
// =============================================================================
#include "ve_lighting.glsl"

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inTangent;
layout(location = 4) in vec4 inColor;
layout(location = 5) flat in float inLodFade;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outGBuffer; // thin G-buffer (see ve.glsl)

// World-space triplanar sample (weights from the geometric normal). Explicit
// gradients: it's called in branches that differ per pixel.
vec4 triplanar(uint tex, vec3 p, vec3 n, float scale, vec3 dPdx, vec3 dPdy)
{
    vec3 w = pow(abs(n), vec3(4.0));
    w /= w.x + w.y + w.z;
    vec3 q = p * scale, qx = dPdx * scale, qy = dPdy * scale;
    vec4 c = vec4(0.0);
    if (w.x > 0.01) c += w.x * veSampleGrad(tex, q.zy, qx.zy, qy.zy);
    if (w.y > 0.01) c += w.y * veSampleGrad(tex, q.xz, qx.xz, qy.xz);
    if (w.z > 0.01) c += w.z * veSampleGrad(tex, q.xy, qx.xy, qy.xy);
    return c;
}

// Painted amount -> mask with broken-up edges: where the grunge is high the
// paint "arrives" first. 0 stays 0 and 1 stays 1 whatever the grunge.
float paintMask(float painted, float grunge, float breakup, float sharpness)
{
    float t = painted * (1.0 + breakup) - breakup * (1.0 - grunge);
    return clamp((t - 0.5) * sharpness + 0.5, 0.0, 1.0);
}

void main()
{
    // Position derivatives (decals, triplanar LODs): here, before any discard.
    vec3 dPdx = dFdx(inWorldPos), dPdy = dFdy(inWorldPos);
    veLodDither(inLodFade);
    VeMaterial m = veMaterial();
    vec2 uv = inUV * m.params[3].xy;

    // ---- Standard surface (as standard.frag, without the vertex colour tint) ----
    vec4 base = m.params[0] * veSampleOr(m.textures[0], uv, vec4(1.0));
    float cutoff = m.params[1].w;
    if (cutoff > 0.0 && base.a < cutoff) discard;

    vec3 N = normalize(inNormal);
    if (!gl_FrontFacing) N = -N;
    vec3 Ng = N;
    if (m.textures[1] != VE_INVALID) {
        vec3 tn = veUnpackNormal(veSample(m.textures[1], uv));
        tn.xy *= m.params[1].z;
        vec3 T = normalize(inTangent.xyz - N * dot(N, inTangent.xyz));
        vec3 B = cross(N, T) * inTangent.w;
        N = normalize(mat3(T, B, N) * tn);
    }
    float roughness = m.params[1].x;
    float metallic = m.params[1].y;
    if (m.textures[2] != VE_INVALID) {
        vec4 mr = veSample(m.textures[2], uv);
        roughness *= mr.g;
        metallic *= mr.b;
    }

    VeSurface s;
    s.position = inWorldPos;
    s.normal = N;
    s.albedo = base.rgb;
    s.roughness = roughness;
    s.metallic = metallic;
    s.ao = m.textures[4] != VE_INVALID ? veSample(m.textures[4], uv).r : 1.0;
    s.emissive = m.params[2].rgb * m.params[2].w * veSampleOr(m.textures[3], uv, vec4(1.0)).rgb;

    // ---- Vertex paint ------------------------------------------------------------
    vec4 strength = m.params[4];
    if (any(greaterThan(strength.xyz, vec3(0.0)))) {
        vec3 painted = clamp(inColor.rgb, 0.0, 1.0);
        painted = mix(painted, 1.0 - painted, step(0.5, m.params[8].xyz)); // invert flags
        float scale = m.params[7].x > 0.0 ? m.params[7].x : 0.5;
        float sharp = m.params[7].y > 0.0 ? max(m.params[7].y, 1.0) : 3.0;
        float grunge = m.textures[6] != VE_INVALID ? triplanar(m.textures[6], inWorldPos, Ng, scale, dPdx, dPdy).r : 0.5;
        float breakup = m.textures[6] != VE_INVALID ? clamp(strength.w, 0.0, 1.0) : 0.0;

        // Wear first (it's under the dirt), then dirt on top, then water over everything.
        float wear = paintMask(painted.g, grunge, breakup, sharp) * strength.y;
        if (wear > 0.0) {
            vec4 wp = m.params[6] == vec4(0.0) ? vec4(0.3, 0.2, 0.13, 0.85) : m.params[6];
            vec3 c = wp.rgb;
            if (m.textures[7] != VE_INVALID) c *= triplanar(m.textures[7], inWorldPos, Ng, scale, dPdx, dPdy).rgb;
            s.albedo = mix(s.albedo, c, wear);
            s.roughness = mix(s.roughness, wp.w, wear);
            s.metallic = mix(s.metallic, 0.0, wear);
        }
        float dirt = paintMask(painted.r, 1.0 - grunge, breakup, sharp) * strength.x;
        if (dirt > 0.0) {
            vec4 dp = m.params[5] == vec4(0.0) ? vec4(0.16, 0.12, 0.09, 0.95) : m.params[5];
            vec3 c = dp.rgb;
            if (m.textures[5] != VE_INVALID) c *= triplanar(m.textures[5], inWorldPos, Ng, scale, dPdx, dPdy).rgb;
            s.albedo = mix(s.albedo, c, dirt);
            s.roughness = mix(s.roughness, dp.w, dirt);
            s.metallic = mix(s.metallic, 0.0, dirt);
            s.ao *= mix(1.0, 0.85, dirt);
        }
        // Water pools in the low (grunge-dark) spots first; soft edge.
        float wet = clamp(painted.b * (1.0 + breakup) - breakup * grunge, 0.0, 1.0) * strength.z;
        if (wet > 0.0) {
            // Porous (rough) surfaces darken more when wet than smooth ones.
            float darken = clamp(m.params[7].z, 0.0, 1.0) * mix(0.4, 1.0, s.roughness);
            s.albedo *= 1.0 - darken * wet;
            s.roughness = mix(s.roughness, m.params[7].w, wet);
            s.normal = normalize(mix(s.normal, Ng, wet * 0.8)); // a film of water fills the bumps
        }
    }

    // Decals (bullet holes, puddles) on top of the paint.
    veApplyDecals(s, Ng, dPdx, dPdy, ~floatBitsToUint(m.params[3].w));

    outColor = vec4(veEvaluateLighting(s), base.a);
    outGBuffer = veGBuffer(s.normal, s.roughness);
}
