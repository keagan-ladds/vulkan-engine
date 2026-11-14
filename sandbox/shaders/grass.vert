#version 460
// Grass: blades sway with the global wind (vfx.glsl: VFX_WIND).
// uv.y = 0 at the root, 1 at the tip (how much the vertex may move);
// uv.x = random phase per clump so the field doesn't move in lockstep.
#include "vfx.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inTangent;
layout(location = 4) in vec4 inColor;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec4 outColor;
layout(location = 2) out float outTip;

void main()
{
    vec4 world = draw.model * vec4(inPosition, 1.0);
    float w = inUV.y * inUV.y;
    vec2 dir = VFX_WIND.xy;
    float strength = VFX_WIND.z;
    float t = frame.time.x;
    // Large rolling gusts travelling across the field + small per-blade flutter.
    float wave = sin(dot(world.xz, dir) * 0.15 - t * 1.6) * 0.5 + 0.5;
    float flutter = sin(t * 5.0 + inUV.x * 30.0 + world.x * 0.7) * 0.25;
    float bend = (0.25 + wave * 0.75) * strength * 0.045 + flutter * 0.05;
    world.xz += dir * bend * w;
    world.y -= abs(bend) * w * 0.3; // bent blades get shorter

    outWorldPos = world.xyz;
    outColor = inColor;
    outTip = inUV.y;
    gl_Position = frame.viewProj * world;
}
