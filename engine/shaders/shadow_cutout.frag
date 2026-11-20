#version 460
// Alpha-tested shadow caster for directional cascades (StandardMaterial
// layout: params[0].a * albedo alpha vs params[1].w cutoff, UV scale params[3].xy).
// Leaves and fences cast shadows with holes instead of solid quads.
#include "ve.glsl"

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec2 inUV;

void main()
{
    veStandardCutout(veMaterial(), inUV);
}
