#version 460
// ve::debug lines: one instance per segment, expanded to a screen-space quad
// (6 vertices) so lines have a thickness in pixels on any GPU.
#include "ve.glsl"

// Push constants: ve.glsl's draw block; draw.model[0][0] = alpha (hidden parts are faint).

layout(location = 0) in vec4 inA;     // xyz, w = thickness (pixels)
layout(location = 1) in vec4 inB;     // xyz
layout(location = 2) in vec4 inColor; // sRGB

layout(location = 0) out vec4 outColor;

void main()
{
    vec4 a = frame.viewProj * vec4(inA.xyz, 1.0);
    vec4 b = frame.viewProj * vec4(inB.xyz, 1.0);
    // Clip against the near side (w > 0) so a segment through the camera still draws.
    const float eps = 1e-3;
    if (a.w < eps && b.w < eps) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0); // off screen
        outColor = vec4(0.0);
        return;
    }
    if (a.w < eps) a = mix(a, b, (eps - a.w) / (b.w - a.w));
    if (b.w < eps) b = mix(b, a, (eps - b.w) / (a.w - b.w));
    const vec2 size = frame.screenSize.xy;
    const vec2 sa = a.xy / a.w * 0.5 * size, sb = b.xy / b.w * 0.5 * size;
    vec2 dir = sb - sa;
    dir = length(dir) > 1e-4 ? normalize(dir) : vec2(1.0, 0.0);
    const vec2 side = vec2(-dir.y, dir.x) * (inA.w * 0.5);
    // Quad corners: (end, side) per vertex of two triangles.
    const int corner[6] = int[6](0, 1, 2, 2, 1, 3);
    const int c = corner[gl_VertexIndex];
    vec4 p = (c & 2) != 0 ? b : a;
    const float s = (c & 1) != 0 ? 1.0 : -1.0;
    p.xy += (side * s) / (0.5 * size) * p.w;
    gl_Position = p;
    // sRGB -> linear (the backbuffer converts back).
    outColor = vec4(pow(inColor.rgb, vec3(2.2)), inColor.a * draw.model[0][0]);
}
