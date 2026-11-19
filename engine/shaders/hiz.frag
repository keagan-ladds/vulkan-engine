#version 460
// Depth pyramid level (ve::HiZPass): each texel holds the farthest depth
// (reversed-Z: the smallest value) of the source texels it covers, so
// "nearer than this" is true for everything drawn in its area.
#define VE_CUSTOM_PUSH_CONSTANTS
#include "ve.glsl"

layout(push_constant) uniform Push {
    uint source;      // SceneDepth or the previous level
    uint pad;
    uvec2 sourceSize;
    uvec2 size;       // this level
} pc;

layout(location = 0) out float outDepth;

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec2 scale = vec2(pc.sourceSize) / vec2(pc.size);
    ivec2 maxS = ivec2(pc.sourceSize) - 1;
    ivec2 lo = clamp(ivec2(floor(vec2(p) * scale)), ivec2(0), maxS);
    ivec2 hi = clamp(ivec2(ceil(vec2(p + 1) * scale)) - 1, lo, min(maxS, lo + 15));
    float farthest = 1.0;
    for (int y = lo.y; y <= hi.y; ++y)
        for (int x = lo.x; x <= hi.x; ++x) farthest = min(farthest, texelFetch(veTextures[nonuniformEXT(pc.source)], ivec2(x, y), 0).r);
    outDepth = farthest;
}
