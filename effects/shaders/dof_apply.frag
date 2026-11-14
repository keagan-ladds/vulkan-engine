#version 460
// Depth of field, composite: blends the blurred buffer over SceneColor by
// each full-resolution pixel's circle of confusion (alpha blend).
#define VE_CUSTOM_PUSH_CONSTANTS
#include "ve.glsl"
#include "dof.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

void main()
{
    float coc = dofCoc(dofDepth(inUV), dofFocusDistance()) * pc.params.z; // pixels
    // Below ~1 pixel of blur the full-res image is sharper than the blurred one.
    float a = smoothstep(0.5, 2.0, coc);
    outColor = vec4(veSampleLod(pc.blurTex, inUV, 0.0).rgb, a);
}
