#version 460
// Impostor baking: writes unlit albedo + alpha and the object-space normal.
// Reads the StandardMaterial parameter layout (base colour, albedo texture,
// vertex colour weight, alpha cutoff), so bake StandardMaterial meshes.
#define VE_CUSTOM_PUSH_CONSTANTS
#include "ve.glsl"

layout(push_constant) uniform Push {
    mat4 viewProj;
    vec4 centreRadius;
    uint materialIndex;
} pc;

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;

layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormal;

void main()
{
    VeMaterial m = materials[pc.materialIndex];
    vec4 base = m.params[0] * veSampleOr(m.textures[0], inUV * m.params[3].xy, vec4(1.0));
    base *= mix(vec4(1.0), inColor, m.params[3].z);
    if (m.params[1].w > 0.0 && base.a < m.params[1].w) discard;
    vec3 n = normalize(inNormal);
    if (!gl_FrontFacing) n = -n;
    outAlbedo = vec4(base.rgb, 1.0);
    outNormal = vec4(n * 0.5 + 0.5, 1.0);
}
