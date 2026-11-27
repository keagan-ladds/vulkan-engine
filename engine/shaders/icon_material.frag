#version 460
// Icon baking with materials: the StandardMaterial layout's base colour,
// albedo texture, vertex colour weight and alpha cutoff (custom shaders using
// that layout look like their base material here), lit like icon.frag.
#define VE_CUSTOM_PUSH_CONSTANTS
#include "ve.glsl"

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 rotation;
    vec4 sunDir;
    vec4 sunColor;
    vec4 sky;
} pc;

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;
layout(location = 0) out vec4 outColor;

void main()
{
    VeMaterial m = materials[floatBitsToUint(pc.sunDir.w)];
    vec4 base = m.params[0] * veSampleOr(m.textures[0], inUV * m.params[3].xy, vec4(1.0));
    base *= mix(vec4(1.0), inColor, m.params[3].z);
    if (m.params[1].w > 0.0 && base.a < m.params[1].w) discard;
    vec3 n = normalize(inNormal);
    if (!gl_FrontFacing) n = -n;
    float sun = max(dot(n, -normalize(pc.sunDir.xyz)), 0.0);
    vec3 ambient = mix(pc.sky.rgb * pc.sky.w, pc.sky.rgb, n.y * 0.5 + 0.5);
    outColor = vec4(base.rgb * (ambient + pc.sunColor.rgb * sun * 1.15), 1.0); // (an sRGB target: linear here)
}
