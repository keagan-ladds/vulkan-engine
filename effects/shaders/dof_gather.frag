#version 460
// Depth of field, gather step (reduced resolution). Each pixel averages
// golden-angle spiral taps of the prep buffer (rgb = colour, a = linear depth); a tap counts only if its own
// circle of confusion reaches this pixel, and taps behind an in-focus pixel
// are limited to that pixel's blur, so sharp foregrounds don't get smeared
// by the background. Output: rgb = blurred colour.
#define VE_CUSTOM_PUSH_CONSTANTS
#include "ve.glsl"
#include "dof.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

void main()
{
    float focus = dofFocusDistance();
    vec4 center = veSampleLod(pc.blurTex, inUV, 0.0);
    float centerDepth = center.a;
    float centerCoc = dofCoc(centerDepth, focus) * pc.params.z; // pixels (full res)

    vec3 sum = center.rgb;
    float wsum = 1.0;
    int taps = int(pc.params2.x);
    float maxRadius = max(pc.params.z, 1.0);
    float rot = veInterleavedGradientNoise(gl_FragCoord.xy) * 6.2831853;
    for (int i = 0; i < taps; ++i) {
        float r = sqrt((float(i) + 0.5) / float(taps)) * maxRadius;
        float a = float(i) * 2.39996323 + rot;
        vec2 uv = inUV + vec2(cos(a), sin(a)) * r * frame.screenSize.zw;
        vec4 tap = veSampleLod(pc.blurTex, uv, 0.0);
        float d = tap.a;
        float coc = dofCoc(d, focus) * pc.params.z;
        // Background taps can't blur over a nearer, sharper pixel.
        if (d > centerDepth) coc = min(coc, centerCoc);
        float w = clamp(coc - r + 1.0, 0.0, 1.0);
        sum += tap.rgb * w;
        wsum += w;
    }
    outColor = vec4(sum / wsum, 1.0);
}
