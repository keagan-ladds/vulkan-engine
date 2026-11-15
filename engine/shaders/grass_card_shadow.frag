#version 460
// Alpha-tested shadow caster for grass cards (GrassPass layout: textures[2]
// card texture, params[5].y alpha cutoff).
#include "ve.glsl"

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec2 inUV;

void main()
{
    VeMaterial m = veMaterial();
    if (veSampleOr(m.textures[2], inUV, vec4(1.0)).a < m.params[5].y) discard;
}
