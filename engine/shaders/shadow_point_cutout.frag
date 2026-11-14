#version 460
// Alpha-tested point-light shadow caster: like shadow_point.frag + cutout test.
#include "ve.glsl"

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec2 inUV;

void main()
{
    VeMaterial m = veMaterial();
    float alpha = m.params[0].a * veSampleOr(m.textures[0], inUV * m.params[3].xy, vec4(1.0)).a;
    if (m.params[1].w > 0.0 && alpha < m.params[1].w) discard;
    VePointLight L = lights.point[draw.lightIndex];
    gl_FragDepth = length(inWorldPos - L.positionRange.xyz) / L.positionRange.w;
}
