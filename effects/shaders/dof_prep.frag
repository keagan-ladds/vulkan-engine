#version 460
// Depth of field, prep step: downsamples SceneColor to the reduced-resolution
// working buffer and packs linear depth into alpha, so the gather reads one
// texture per tap.
#define VE_CUSTOM_PUSH_CONSTANTS
#include "ve.glsl"
#include "dof.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

void main()
{
    // Nearest depth of the 2x2 footprint keeps silhouettes on the foreground side.
    vec2 o = 0.5 * frame.screenSize.zw;
    float d = min(min(dofDepth(inUV + vec2(-o.x, -o.y)), dofDepth(inUV + vec2(o.x, -o.y))),
                  min(dofDepth(inUV + vec2(-o.x, o.y)), dofDepth(inUV + vec2(o.x, o.y))));
    outColor = vec4(veSampleLod(pc.colorTex, inUV, 0.0).rgb, d);
}
