#version 460
#include "ve_lighting.glsl"
#include "ve_impostor.glsl"

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inObjPos;
layout(location = 2) flat in vec3 inObjCamera;
layout(location = 3) flat in mat3 inRotation;
layout(location = 6) flat in vec4 inTint;
layout(location = 7) flat in float inLodFade;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outGBuffer; // thin G-buffer (see ve.glsl)

void main()
{
    veLodDither(inLodFade);
    VeMaterial m = veMaterial();
    vec3 centre = m.params[0].xyz;
    float radius = m.params[0].w;
    float N = m.params[1].x;

    // Which frames see the object from (about) our direction?
    vec3 v = inObjCamera - centre;
    v.y = max(v.y, 0.0);
    v = length(v) > 1e-5 ? normalize(v) : vec3(0.0, 1.0, 0.0);
    ivec2 f[3];
    vec3 w;
    veImpostorFrames(v, N, f[0], f[1], f[2], w);

    // Blend the three frames, each sampled where our view ray crosses its plane.
    vec3 dir = normalize(inObjPos - inObjCamera);
    vec4 albedo = vec4(0.0);
    vec3 normal = vec3(0.0);
    for (int k = 0; k < 3; ++k) {
        bool inside;
        vec2 uv = veImpostorFrameUV(f[k], N, inObjCamera, dir, centre, radius, inside);
        if (!inside) continue;
        vec4 a = veSample(m.textures[0], uv);
        // Weight by alpha (premultiplied) so empty texels don't bleed their
        // background colour into the blend.
        albedo += vec4(a.rgb * a.a, a.a) * w[k];
        normal += (veSample(m.textures[1], uv).xyz * 2.0 - 1.0) * w[k] * a.a;
    }
    if (albedo.a < m.params[1].y) discard;
    vec3 rgb = albedo.rgb / max(albedo.a, 1e-4);
    if (inTint.w > 0.0) rgb *= inTint.rgb; // same per-instance tint as standard.vert

    VeSurface s = veDefaultSurface(inWorldPos, normalize(inRotation * normalize(normal + vec3(0, 1e-4, 0))), rgb);
    s.roughness = 0.85;
    outColor = vec4(veEvaluateLighting(s), 1.0);
    outGBuffer = veGBuffer(s.normal, s.roughness);
}
