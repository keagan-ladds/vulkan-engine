#version 460
// Standard PBR material. Parameter layout: see ve::StandardMaterial.
#include "ve_lighting.glsl"

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inTangent;
layout(location = 4) in vec4 inColor;
layout(location = 5) flat in float inLodFade;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outGBuffer; // thin G-buffer (see ve.glsl)

void main()
{
    veLodDither(inLodFade);
    VeMaterial m = veMaterial();
    vec2 uv = inUV * m.params[3].xy;

    vec4 base = m.params[0] * veSampleOr(m.textures[0], uv, vec4(1.0));
    base *= mix(vec4(1.0), inColor, m.params[3].z); // vertex colour
    float cutoff = m.params[1].w;
    if (cutoff > 0.0 && base.a < cutoff) discard;

    vec3 N = normalize(inNormal);
    if (!gl_FrontFacing) N = -N; // double-sided materials light their back side correctly
    if (m.textures[1] != VE_INVALID) {
        vec3 tn = veSample(m.textures[1], uv).xyz * 2.0 - 1.0;
        tn.xy *= m.params[1].z;
        vec3 T = normalize(inTangent.xyz - N * dot(N, inTangent.xyz));
        vec3 B = cross(N, T) * inTangent.w;
        N = normalize(mat3(T, B, N) * tn);
    }

    float roughness = m.params[1].x;
    float metallic = m.params[1].y;
    if (m.textures[2] != VE_INVALID) {
        vec4 mr = veSample(m.textures[2], uv);
        roughness *= mr.g; // glTF: G = roughness, B = metallic
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

    vec3 color = veEvaluateLighting(s);

    // Translucency (thin leaves): directional light from behind the surface
    // shines through it, tinted by the albedo. A soft wrap term for light
    // anywhere behind, and forward scattering when looking towards the light.
    float translucency = m.params[3].w;
    if (translucency > 0.0 && (frame.lightCounts.w & 2u) == 0u) {
        vec3 V = normalize(frame.cameraPosition.xyz - inWorldPos);
        float viewDepth = dot(inWorldPos - frame.cameraPosition.xyz, frame.cameraForward.xyz);
        vec3 tint = m.params[4].rgb * s.albedo * (1.0 - metallic);
        for (uint i = 0; i < frame.lightCounts.x; ++i) {
            VeDirectionalLight L = lights.directional[i];
            vec3 l = -L.direction.xyz;
            float back = dot(-N, l);
            if (back <= 0.0) continue; // lit from the front: the regular lighting has it
            float shadow = veDirectionalShadow(i, inWorldPos, -N, viewDepth); // (the lit side faces away)
            // A crown shadows its own far side, but light still diffuses leaf to leaf through it:
            // some of the soft back term survives the shadow, the bright forward lobe (looking
            // towards the light) mostly doesn't.
            float scatter = back * 0.7 * mix(0.3, 1.0, shadow) + pow(max(dot(-V, l), 0.0), 4.0) * 2.0 * mix(0.1, 1.0, shadow);
            color += tint * L.color.rgb * scatter * translucency * (1.0 / 3.14159265);
        }
    }
    outColor = vec4(color, base.a);
    outGBuffer = veGBuffer(s.normal, s.roughness);
}
