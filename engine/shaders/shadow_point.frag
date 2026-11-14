#version 460
// Point-light shadow: store distance-to-light / range as depth. Lookups in
// vePointShadow() compare against the same quantity, so it doesn't matter
// which cube face (or projection) produced it.
#include "ve.glsl"

layout(location = 0) in vec3 inWorldPos;

void main()
{
    VePointLight L = lights.point[draw.lightIndex];
    gl_FragDepth = length(inWorldPos - L.positionRange.xyz) / L.positionRange.w;
}
