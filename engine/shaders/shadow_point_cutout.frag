#version 460
// Alpha-tested point-light shadow caster: like shadow_point.frag + cutout test.
#include "ve.glsl"

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec2 inUV;

void main()
{
    veStandardCutout(veMaterial(), inUV);
    VePointLight L = lights.point[draw.lightIndex];
    gl_FragDepth = length(inWorldPos - L.positionRange.xyz) / L.positionRange.w;
}
