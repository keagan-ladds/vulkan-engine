#version 460
// Unlit material: colour * texture * vertex colour + emissive. Same
// parameter layout as the standard material (params[0], [2], [3], textures[0]).
#include "ve.glsl"

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
    vec4 c = m.params[0] * veSampleOr(m.textures[0], uv, vec4(1.0)) * mix(vec4(1.0), inColor, m.params[3].z);
    outColor = vec4(c.rgb + m.params[2].rgb * m.params[2].w, c.a);
}
