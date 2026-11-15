#version 460
// Default GrassPass vertex shader: places the clump (ve_grass.glsl), procedural
// blades or a card mesh (params[5].x), and sways the tips with the wind in
// frame.globalParams[windSlot].
#include "ve_grass.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inTangent;
layout(location = 4) in vec4 inColor;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec4 outColor; // rgb colour, a = mask.w (free channel)
layout(location = 2) out float outTip; // sway weight: 0 root .. 1 tip
layout(location = 3) out vec2 outUV;   // card texture coordinates (cards only)

void main()
{
    VeMaterial m = veMaterial();
    VeGrassClump c = veGrassClump(m);
    if (!c.visible) {
        gl_Position = vec4(0.0, 0.0, -1.0, 1.0); // outside the depth range: clipped
        return;
    }
    bool cards = m.params[5].x > 0.5;
    vec3 world = cards ? veGrassCardPosition(c, inPosition) : veGrassVertexPosition(c, inPosition, inTangent.xyz, inUV, inColor.r);
    float tip = cards ? clamp(inPosition.y, 0.0, 1.0) : inUV.y;
    float phase = cards ? veHash12(c.root.xz) : inUV.x;

    int windSlot = int(m.params[4].y);
    if (windSlot >= 0) {
        vec4 wind = frame.globalParams[windSlot];
        float w = tip * tip * m.params[4].z;
        float t = frame.time.x;
        // Rolling gusts across the field + per-blade flutter.
        float wave = sin(dot(world.xz, wind.xy) * 0.15 - t * 1.6) * 0.5 + 0.5;
        float flutter = sin(t * 5.0 + phase * 30.0 + world.x * 0.7) * 0.25;
        float bend = ((0.25 + wave * 0.75) * wind.z * 0.045 + flutter * 0.05) * c.height;
        world.xz += wind.xy * bend * w;
        world.y -= abs(bend) * w * 0.3;
    }

    outWorldPos = world;
    outColor = vec4(cards ? c.color : c.color * mix(0.7, 1.15, tip), c.mask.w);
    outTip = tip;
    outUV = inUV;
    gl_Position = frame.viewProj * vec4(world, 1.0);
}
