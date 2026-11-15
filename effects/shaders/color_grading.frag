#version 460
// Colour grading + tonemapping (vfx::ColorGradingPass). HDR SceneColor in,
// graded and tonemapped LINEAR colour (0..1) out; CompositePass then only adds
// vignette and dither.
//
// Order: exposure -> white balance -> colour filter -> contrast (log, around
// mid grey) -> saturation / vibrance -> tonemap -> (display space) split
// toning -> lift / gamma / gain -> optional LUT -> linear.
#define VE_CUSTOM_PUSH_CONSTANTS
#include "ve.glsl"

layout(push_constant) uniform Push {
    vec4 whiteBalance; // rgb = LMS scale, w = exposure (linear multiplier)
    vec4 colorFilter;  // rgb = colour filter, w = contrast
    vec4 shadows;      // rgb = shadow tint, w = saturation
    vec4 highlights;   // rgb = highlight tint, w = split-toning balance (-1..1)
    vec4 lift;         // rgb, w = vibrance
    vec4 gamma;        // rgb, w = white point (filmic/Reinhard)
    vec4 gain;         // rgb, w = LUT contribution
    uint sceneColor;
    uint tonemapper;   // 0 ACES fitted, 1 ACES (Hill), 2 AgX, 3 Filmic (Hable), 4 Reinhard, 5 Khronos neutral, 6 none
    uint lut;          // strip LUT (size^2 x size), VE_INVALID = none
    uint lutSize;
} pc;

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

// --- White balance (LMS, as in Unity's post stack) -------------------------------
const mat3 LIN_2_LMS = mat3(3.90405e-1, 7.08416e-2, 2.31082e-2,
                            5.49941e-1, 9.63172e-1, 1.28021e-1,
                            8.92632e-3, 1.35775e-3, 9.36245e-1);
const mat3 LMS_2_LIN = mat3(2.85847e+0, -2.10182e-1, -4.18120e-2,
                            -1.62879e+0, 1.15820e+0, -1.18169e-1,
                            -2.48910e-2, 3.24281e-4, 1.06867e+0);

// --- Tonemappers -----------------------------------------------------------------
vec3 acesFitted(vec3 x) // Narkowicz
{
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

vec3 acesHill(vec3 c) // Stephen Hill's RRT+ODT fit
{
    const mat3 inputMat = mat3(0.59719, 0.07600, 0.02840, 0.35458, 0.90834, 0.13383, 0.04823, 0.01566, 0.83777);
    const mat3 outputMat = mat3(1.60475, -0.10208, -0.00327, -0.53108, 1.10813, -0.07276, -0.07367, -0.00605, 1.07602);
    c = inputMat * c;
    vec3 a = c * (c + 0.0245786) - 0.000090537;
    vec3 b = c * (0.983729 * c + 0.4329510) + 0.238081;
    return clamp(outputMat * (a / b), 0.0, 1.0);
}

vec3 agx(vec3 c) // Troy Sobotka's AgX, minimal fit; returns linear
{
    const mat3 inset = mat3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
                            0.0784335999999992, 0.878468636469772, 0.0784336,
                            0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const mat3 outset = mat3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
                             -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
                             -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float minEv = -12.47393, maxEv = 4.026069;
    c = clamp(log2(max(inset * c, 1e-10)), minEv, maxEv);
    c = (c - minEv) / (maxEv - minEv);
    vec3 x2 = c * c, x4 = x2 * x2;
    c = 15.5 * x4 * x2 - 40.14 * x4 * c + 31.96 * x4 - 6.868 * x2 * c + 0.4298 * x2 + 0.1191 * c - 0.00232;
    return pow(clamp(outset * c, 0.0, 1.0), vec3(2.2));
}

vec3 hablePartial(vec3 x)
{
    const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

vec3 filmic(vec3 c, float white) // John Hable (Uncharted 2): the 2008-era filmic look
{
    return clamp(hablePartial(c * 2.0) / hablePartial(vec3(white)), 0.0, 1.0);
}

vec3 reinhardExtended(vec3 c, float white)
{
    float l = veLuminance(c);
    float lt = l * (1.0 + l / (white * white)) / (1.0 + l);
    return clamp(c * (lt / max(l, 1e-5)), 0.0, 1.0);
}

vec3 khronosNeutral(vec3 color)
{
    const float startCompression = 0.8 - 0.04, desaturation = 0.15;
    float x = min(color.r, min(color.g, color.b));
    color -= x < 0.08 ? x - 6.25 * x * x : 0.04;
    float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression) return color;
    const float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(color, vec3(newPeak), g);
}

vec3 tonemap(vec3 c)
{
    switch (pc.tonemapper) {
    case 0u: return acesFitted(c);
    case 1u: return acesHill(c);
    case 2u: return agx(c);
    case 3u: return filmic(c, pc.gamma.w);
    case 4u: return reinhardExtended(c, pc.gamma.w);
    case 5u: return clamp(khronosNeutral(c), 0.0, 1.0);
    default: return clamp(c, 0.0, 1.0);
    }
}

// Strip LUT: size slices of size x size side by side (blue picks the slice).
vec3 applyLut(vec3 g)
{
    float n = float(pc.lutSize);
    float b = g.b * (n - 1.0);
    float s0 = floor(b), s1 = min(s0 + 1.0, n - 1.0);
    vec2 uv = vec2((g.r * (n - 1.0) + 0.5) / (n * n), (g.g * (n - 1.0) + 0.5) / n);
    vec3 c0 = veSampleLod(pc.lut, uv + vec2(s0 / n, 0.0), 0.0).rgb;
    vec3 c1 = veSampleLod(pc.lut, uv + vec2(s1 / n, 0.0), 0.0).rgb;
    return mix(c0, c1, b - s0);
}

void main()
{
    vec3 c = max(veSampleLod(pc.sceneColor, inUV, 0.0).rgb, 0.0) * pc.whiteBalance.w;

    // White balance + colour filter.
    c = LMS_2_LIN * ((LIN_2_LMS * c) * pc.whiteBalance.rgb);
    c *= pc.colorFilter.rgb;

    // Contrast in log2 space around 18 % grey (keeps mid-tones put).
    const float midGrey = log2(0.18);
    c = exp2((log2(max(c, 1e-6)) - midGrey) * pc.colorFilter.w + midGrey);

    // Saturation and vibrance (vibrance boosts dull colours more than vivid ones).
    float l = veLuminance(c);
    c = max(mix(vec3(l), c, pc.shadows.w), 0.0);
    float sat = (max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b))) / max(max(c.r, max(c.g, c.b)), 1e-5);
    c = max(mix(vec3(l), c, 1.0 + pc.lift.w * (1.0 - sat)), 0.0);

    c = tonemap(c);

    // Display-space grading (gamma 2.2), intuitive for artists.
    vec3 g = pow(clamp(c, 0.0, 1.0), vec3(1.0 / 2.2));
    // Split toning: tint shadows and highlights separately.
    float t = smoothstep(0.0, 1.0, clamp(veLuminance(g) + pc.highlights.w * 0.5, 0.0, 1.0));
    g *= mix(pc.shadows.rgb, pc.highlights.rgb, t);
    // Lift / gamma / gain (ASC CDL-like).
    g = g * pc.gain.rgb + pc.lift.rgb * (1.0 - g);
    g = pow(max(g, 0.0), 1.0 / max(pc.gamma.rgb, vec3(0.01)));
    g = clamp(g, 0.0, 1.0);
    if (pc.lut != VE_INVALID) g = mix(g, applyLut(g), pc.gain.w);

    outColor = vec4(pow(g, vec3(2.2)), 1.0);
}
