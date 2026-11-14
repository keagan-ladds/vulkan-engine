#version 460
// HDR scene colour -> display: exposure, tonemapping, grading, vignette.
// Output is linear; the sRGB swapchain applies the gamma encoding.
#define VE_CUSTOM_PUSH_CONSTANTS
#include "ve.glsl"

layout(push_constant) uniform Push {
    uint sceneColor;
    float exposure;
    float contrast;
    float saturation;
    vec4 tint; // rgb, w = vignette strength
    int tonemapper;
} pc;

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

// Narkowicz's fitted ACES curve: filmic contrast + highlight roll-off.
vec3 acesFitted(vec3 x)
{
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main()
{
    vec3 c = veSample(pc.sceneColor, inUV).rgb * pc.exposure * pc.tint.rgb;

    if (pc.tonemapper == 0) c = acesFitted(c);
    else if (pc.tonemapper == 1) c = c / (1.0 + c);
    c = clamp(c, 0.0, 1.0);

    // Grade in a roughly perceptual space (gamma 2.2), then back to linear.
    vec3 g = pow(c, vec3(1.0 / 2.2));
    g = (g - 0.5) * pc.contrast + 0.5;
    float l = veLuminance(g);
    g = mix(vec3(l), g, pc.saturation);
    vec2 d = inUV - 0.5;
    g *= 1.0 - pc.tint.w * smoothstep(0.2, 0.8, dot(d, d) * 2.0);
    // Tiny dither to hide banding in skies/fog.
    g += (veInterleavedGradientNoise(gl_FragCoord.xy) - 0.5) / 255.0;
    c = pow(clamp(g, 0.0, 1.0), vec3(2.2));

    outColor = vec4(c, 1.0);
}
