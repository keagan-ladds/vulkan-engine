#version 460
// Standard PBR material. Parameter layout: see ve::StandardMaterial.
#include "ve_lighting.glsl"

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inTangent;
layout(location = 4) in vec4 inColor;

layout(location = 0) out vec4 outColor;

void main()
{
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

    outColor = vec4(veEvaluateLighting(s), base.a);
}
