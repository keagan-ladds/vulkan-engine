#version 460
// Camera-facing particle quads. No vertex buffer for the quad itself: the 6
// corners come from gl_VertexIndex; per-particle data is an instance stream.
#define VE_CUSTOM_PUSH_CONSTANTS
#include "ve.glsl"

layout(push_constant) uniform Push {
    uint texture;
    uint flags;     // 1 = stretch along velocity
    float stretch;  // seconds of motion the streak covers
    float softness; // soft-particle fade distance (m)
} pc;

layout(location = 0) in vec4 inPosSize;  // xyz, size
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec4 inVelRot;   // xyz velocity, w rotation (radians)

layout(location = 0) out vec2 outUV;
layout(location = 1) out vec4 outColor;
layout(location = 2) out float outViewDepth;

void main()
{
    const vec2 corners[6] = vec2[6](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));
    vec2 c = corners[gl_VertexIndex];
    outUV = c * vec2(0.5, -0.5) + 0.5;
    outColor = inColor;

    vec3 camRight = vec3(frame.view[0][0], frame.view[1][0], frame.view[2][0]);
    vec3 camUp = vec3(frame.view[0][1], frame.view[1][1], frame.view[2][1]);
    vec3 center = inPosSize.xyz;
    float size = inPosSize.w;
    vec3 world;
    if ((pc.flags & 1u) != 0u) {
        // Velocity-aligned streak (rain, sparks).
        vec3 toCam = normalize(frame.cameraPosition.xyz - center);
        vec3 v = inVelRot.xyz * pc.stretch;
        vec3 side = normalize(cross(v + vec3(1e-5), toCam)) * size;
        world = center + side * c.x + v * (c.y * 0.5);
    } else {
        float s = sin(inVelRot.w), co = cos(inVelRot.w);
        vec2 r = vec2(c.x * co - c.y * s, c.x * s + c.y * co);
        world = center + (camRight * r.x + camUp * r.y) * size;
    }
    vec4 view = frame.view * vec4(world, 1.0);
    outViewDepth = -view.z;
    gl_Position = frame.proj * view;
}
