#version 460
// Procedural flames: rising, domain-warped value noise inside a teardrop,
// eroded into tongues at the top, coloured by a black-body-like ramp.
// Additive HDR output (bloom turns it into glow).
#include "ve.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 1) flat in vec4 inShape;   // radius, intensity, seed, kind
layout(location = 2) flat in vec4 inParams;  // temperature, time, sheets
layout(location = 3) in float inViewDepth;

layout(location = 0) out vec4 outColor;

float fbm(vec3 p)
{
    float s = 0.0, a = 0.5;
    for (int i = 0; i < 4; ++i) {
        s += a * veValueNoise(p);
        p = p * 2.03 + vec3(1.7, 9.2, 3.1);
        a *= 0.5;
    }
    return s / 0.9375; // 0..1
}

// Heat 0..1 -> colour: embers (deep red), orange, yellow, near white.
vec3 fireColor(float heat)
{
    return vec3(1.0, 0.2, 0.025) * smoothstep(0.0, 0.35, heat) +
           vec3(0.0, 0.36, 0.05) * smoothstep(0.3, 0.75, heat) +
           vec3(0.0, 0.14, 0.16) * smoothstep(0.75, 1.1, heat);
}

void main()
{
    float intensity = inShape.y, seed = inShape.z;
    float temperature = inParams.x, t = inParams.y;
    vec3 col;
    float softness;
    if (inShape.w < 0.5) {
        vec2 uv = inUV; // x in radii (0 = centre), y in heights (0 = base, 1 = tips)
        float y = uv.y;
        // Large slow turbulence rising with the hot air...
        float n1 = fbm(vec3(uv.x * 0.9, y * 1.7 - t * 1.9, seed + t * 0.25));
        // ...sways the flame more the higher it gets...
        float x = uv.x + (n1 - 0.5) * 1.5 * y;
        // ...and faster detail tears the top into tongues and wisps.
        float n2 = fbm(vec3(x * 2.1 + n1 * 1.3, y * 3.3 - t * 3.6, seed * 1.31 + t * 0.45));
        // Teardrop: rounded at the fuel, widest just above, tapering to the tips.
        float w = mix(0.75, 1.0, smoothstep(0.0, 0.2, y)) * pow(clamp(1.0 - y / 1.08, 0.0, 1.0), 0.45);
        float body = 1.0 - smoothstep(w * 0.2, w, abs(x));
        // Separate tongues of different heights: a slow noise across the flame.
        float tongues = fbm(vec3(x * 1.9, y * 0.5 - t * 0.9, seed * 0.77 + t * 0.2));
        body *= smoothstep(0.25, 0.7, tongues + 0.42 - y * 0.4);
        float heat = body * (0.35 + 1.15 * n2) - y * 0.7;
        heat += body * (1.0 - smoothstep(0.0, 0.3, y)) * 0.35; // hottest near the fuel
        heat = clamp(heat * 1.25 * temperature, 0.0, 1.15);
        float alpha = smoothstep(0.03, 0.35, heat);
        col = fireColor(heat) * alpha;
        // Clean combustion right at the fuel burns faintly blue.
        col += vec3(0.04, 0.10, 0.45) * body * (1.0 - smoothstep(0.0, 0.07, y)) * 0.35;
        // Sheets overlap: keep the total brightness independent of their number.
        col *= intensity * (1.6 / (inParams.z + 0.6));
        softness = 0.12;
    } else {
        vec2 uv = inUV; // -1..1 over the coal bed
        float r = length(uv);
        float n = fbm(vec3(uv * 3.5, seed + t * 0.12));
        float glow = fbm(vec3(uv * 9.0 + 7.0, seed * 0.7 + t * 0.55)); // embers breathing
        float mask = 1.0 - smoothstep(0.3, 1.0, r + (n - 0.5) * 0.6);
        float heat = mask * (0.12 + 0.5 * smoothstep(0.4, 0.8, glow)) * (0.8 + 0.4 * n) * temperature;
        col = fireColor(heat) * mask * intensity * 0.5;
        softness = 0.04;
    }
    // Soft where it meets geometry (logs, stones, ground) and near the camera.
    vec2 screenUV = gl_FragCoord.xy * frame.screenSize.zw;
    float sceneDepth = veLinearDepth(veSampleLod(frame.builtinTextures.x, screenUV, 0.0).r);
    float fade = clamp((sceneDepth - inViewDepth) / softness, 0.0, 1.0) * clamp((inViewDepth - 0.2) / 0.5, 0.0, 1.0);
    col *= fade;
    if (max(col.r, max(col.g, col.b)) <= 1e-4) discard;
    outColor = vec4(col, 1.0);
}
