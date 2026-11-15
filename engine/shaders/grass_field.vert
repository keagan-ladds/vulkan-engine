#version 460
// Default GrassPass vertex shader: places the clump (ve_grass.glsl: procedural
// blades or a card mesh, plus wind). grass_shadow.vert does the same for the
// shadow cascades.
#include "ve_grass.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inTangent;
layout(location = 4) in vec4 inColor;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec4 outColor; // rgb colour, a = mask.w (free channel)
layout(location = 2) out float outTip;  // sway weight: 0 root .. 1 tip
layout(location = 3) out vec2 outUV;    // card texture coordinates (cards only)

void main()
{
    VeMaterial m = veMaterial();
    VeGrassClump c = veGrassClump(m);
    if (!c.visible) {
        gl_Position = vec4(0.0, 0.0, -1.0, 1.0); // all vertices collapse: zero-area triangles
        return;
    }
    float tip;
    vec3 world = veGrassPlace(m, c, inPosition, inTangent, inUV, inColor, tip);
    bool cards = m.params[5].x > 0.5;
    outWorldPos = world;
    outColor = vec4(cards ? c.color : c.color * mix(0.7, 1.15, tip), c.mask.w);
    outTip = tip;
    outUV = inUV;
    gl_Position = frame.viewProj * vec4(world, 1.0);
}
