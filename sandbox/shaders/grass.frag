#version 460
#include "ve_lighting.glsl"
#include "vfx.glsl"

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec4 inColor;
layout(location = 2) in float inTip;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outGBuffer; // thin G-buffer (see ve.glsl)

void main()
{
    // Grass is lit as if its normal pointed up (like the ground it grows
    // from); this keeps a field of blades soft instead of noisy.
    VeSurface s = veDefaultSurface(inWorldPos, vec3(0.0, 1.0, 0.0), inColor.rgb * mix(1.0, 0.65, VFX_WEATHER.x));
    s.roughness = 0.9;
    s.ao = mix(0.75, 1.0, inTip);
    vec3 color = veEvaluateLighting(s);

    // Back-lit translucency: blades glow when the low sun is behind them.
    vec3 V = normalize(frame.cameraPosition.xyz - inWorldPos);
    float back = pow(clamp(dot(-V, VFX_SUN.xyz), 0.0, 1.0), 6.0);
    color += VFX_SUN_COLOR.rgb * back * inColor.rgb * inTip * 0.12 * vfxSunShadow(inWorldPos);
    outColor = vec4(color, 1.0);
    outGBuffer = veGBuffer(vec3(0.0, 1.0, 0.0), 0.9);
}
