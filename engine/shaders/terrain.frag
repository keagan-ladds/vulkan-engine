#version 460
// ve::Terrain material:
//   params[0].xy = detail texture scales (near, far), params[1].rgb = rock colour,
//   params[1].w = global param slot holding wetness in .x (-1 = none; the
//   effects' weather uses slot 1), params[2].x = water level
//   textures[0]  = greyscale detail noise
//   params[3].xy = LOD morph range (ve_terrain.glsl)
// Biome colours come from the vertex colour (computed by ve::Terrain).
#include "ve_lighting.glsl"

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inColor;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outGBuffer; // thin G-buffer (see ve.glsl)

void main()
{
    VeMaterial m = veMaterial();
    vec3 N = normalize(inNormal);

    // Two scales of detail break up the flat vertex colours.
    float d1 = veSample(m.textures[0], inWorldPos.xz * m.params[0].x).r;
    float d2 = veSample(m.textures[0], inWorldPos.xz * m.params[0].y).r;
    float detail = mix(0.78, 1.22, d1 * 0.6 + d2 * 0.4);
    vec3 albedo = inColor.rgb * detail;

    // Steep faces become bare rock, whatever the biome.
    float steep = smoothstep(0.35, 0.6, 1.0 - N.y);
    albedo = mix(albedo, m.params[1].rgb * mix(0.7, 1.15, d1), steep);

    // Wetness: near the waterline, and everywhere when it rains.
    float shore = smoothstep(0.8, 0.0, inWorldPos.y - m.params[2].x);
    float wet = clamp(max(shore, (int(m.params[1].w) >= 0 ? frame.globalParams[int(m.params[1].w)].x : 0.0) * 0.8), 0.0, 1.0);
    albedo *= mix(1.0, 0.55, wet);

    VeSurface s = veDefaultSurface(inWorldPos, N, albedo);
    s.roughness = mix(0.95, 0.35, wet);
    s.ao = mix(0.75, 1.0, d2);
    outColor = vec4(veEvaluateLighting(s), 1.0);
    outGBuffer = veGBuffer(N, s.roughness);
}
