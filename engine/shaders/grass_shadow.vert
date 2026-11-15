#version 460
// GrassPass shadow caster (directional cascades): the same clump placement and
// wind as grass_field.vert, projected with lights.shadowViews[draw.viewIndex].
// Follows the shadow.vert contract (world position at 0, uv at 1).
#include "ve_grass.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inTangent;
layout(location = 4) in vec4 inColor;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec2 outUV;

void main()
{
    VeMaterial m = veMaterial();
    VeGrassClump c = veGrassClump(m);
    if (!c.visible) {
        gl_Position = vec4(0.0, 0.0, 0.5, 1.0); // zero-area triangles (depth clamp: no z clipping)
        return;
    }
    float tip;
    vec3 world = veGrassPlace(m, c, inPosition, inTangent, inUV, inColor, tip);
    outWorldPos = world;
    outUV = inUV;
    gl_Position = lights.shadowViews[draw.viewIndex] * vec4(world, 1.0);
}
