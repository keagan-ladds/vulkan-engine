#version 460
// ve::Terrain material:
//   params[0].xy = detail texture scales (near, far)
//   params[1].w  = global param slot holding wetness in .x (-1 = none; the effects' weather uses slot 1)
//   params[2].x  = water level, params[2].y = index of the rock layer (projected from three sides)
//   params[3].xy = LOD morph range (ve_terrain.glsl)
//   textures[0]  = greyscale detail noise
//   textures[1]  = splat layer table (RGBA32F, a row per layer: the biomes, then rock):
//                  texel 0 = albedo texture, surface texture (-1 = none), 1 / size, normal strength
//                  texel 1 = tint rgb, roughness without a map
//                  texel 2 = x: biome colour amount (the vertex colour carrying the albedo's brightness detail)
//                  Surface textures pack normal x/y, roughness, AO.
// Each vertex blends up to three splat layers (terrain.vert); the weight left
// over shows the biome vertex colour, broken up by the detail noise.
#include "ve_lighting.glsl"

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;
layout(location = 3) in vec3 inSplat;
layout(location = 4) flat in uint inLayers;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outGBuffer; // thin G-buffer (see ve.glsl)

struct Layer {
    vec3 albedo;
    float height;  // albedo alpha: sharpens borders between layers
    vec3 bump;     // world-space normal offset
    float roughness;
    float ao;
};

// Gradients are taken once up front: layers are sampled in divergent branches.
vec3 gDx, gDy;

// One planar sample of a layer's two textures (normal offset along axes t, b).
void sampleLayer(vec4 t0, vec3 base, vec2 uv, vec2 dx, vec2 dy, vec3 t, vec3 b, float w, inout Layer l)
{
    vec4 a = t0.x >= 0.0 ? veSampleGrad(uint(t0.x), uv, dx, dy) : vec4(base, 0.5);
    vec4 s = t0.y >= 0.0 ? veSampleGrad(uint(t0.y), uv, dx, dy) : vec4(0.5, 0.5, -1.0, 1.0);
    vec2 n = (s.xy * 2.0 - 1.0) * t0.w;
    l.albedo += a.rgb * w;
    l.height += a.a * w;
    l.bump += (t * n.x + b * n.y) * w;
    l.roughness += s.z * w; // -1 = no roughness map (resolved by the caller)
    l.ao += s.w * w;
}

Layer layer(uint table, uint row, bool triplanar, vec3 N, vec3 base, float dist)
{
    vec4 t0 = texelFetch(veTextures[nonuniformEXT(table)], ivec2(0, row), 0);
    vec4 t1 = texelFetch(veTextures[nonuniformEXT(table)], ivec2(1, row), 0);
    float biomeColor = texelFetch(veTextures[nonuniformEXT(table)], ivec2(2, row), 0).x;
    Layer l = Layer(vec3(0.0), 0.0, vec3(0.0), 0.0, 0.0);
    float k = t0.z; // repeats per metre
    vec3 p = inWorldPos;
    // Beyond ~40-150 repeats even the anti-tiling below would show as a
    // pattern (the texture's own large features), so the layer fades to its
    // average colour, flat; the macro noise in main() varies that instead.
    float repeats = dist * k;
    float flatten = smoothstep(40.0, 150.0, repeats);
    if (flatten < 1.0 && triplanar) {
        // Rock: projected along each axis, weighted by how much the surface faces it.
        vec3 bw = pow(abs(N), vec3(4.0));
        bw *= vec3(greaterThan(bw / (bw.x + bw.y + bw.z), vec3(0.02)));
        bw *= (1.0 - flatten) / (bw.x + bw.y + bw.z);
        if (bw.x > 0.0) sampleLayer(t0, base, vec2(p.z, -p.y) * k, vec2(gDx.z, -gDx.y) * k, vec2(gDy.z, -gDy.y) * k, vec3(0, 0, 1), vec3(0, 1, 0), bw.x, l);
        if (bw.y > 0.0) sampleLayer(t0, base, p.xz * k, gDx.xz * k, gDy.xz * k, vec3(1, 0, 0), vec3(0, 0, -1), bw.y, l);
        if (bw.z > 0.0) sampleLayer(t0, base, vec2(p.x, -p.y) * k, vec2(gDx.x, -gDx.y) * k, vec2(gDy.x, -gDy.y) * k, vec3(1, 0, 0), vec3(0, 1, 0), bw.z, l);
    } else if (flatten < 1.0) {
        // Anti-tiling: further away, a second rotated, 3.7x larger copy is
        // blended in, so the repeats of the first stop lining up.
        vec3 T = normalize(vec3(1, 0, 0) - N * N.x), B = normalize(vec3(0, 0, -1) + N * N.z);
        float far = clamp((repeats - 6.0) / 30.0, 0.0, 1.0) * 0.5;
        sampleLayer(t0, base, p.xz * k, gDx.xz * k, gDy.xz * k, T, B, (1.0 - far) * (1.0 - flatten), l);
        if (far > 0.0) {
            const mat2 R = mat2(0.8, 0.6, -0.6, 0.8);
            float k2 = k * 0.27;
            sampleLayer(t0, base, R * p.xz * k2 + 0.31, R * gDx.xz * k2, R * gDy.xz * k2, T, B, far * (1.0 - flatten), l);
        }
    }
    if (flatten > 0.0) {
        vec4 a = t0.x >= 0.0 ? veSampleLod(uint(t0.x), vec2(0.5), 16.0) : vec4(base, 0.5);
        vec4 s = t0.y >= 0.0 ? veSampleLod(uint(t0.y), vec2(0.5), 16.0) : vec4(0.5, 0.5, -1.0, 1.0);
        l.albedo += a.rgb * flatten;
        l.height += a.a * flatten;
        l.roughness += s.z * flatten;
        l.ao += s.w * flatten;
    }
    if (biomeColor > 0.0 && t0.x >= 0.0) {
        // The biome's colour, lightened / darkened like the texture around its average.
        const vec3 lum = vec3(0.2126, 0.7152, 0.0722);
        float average = max(dot(veSampleLod(uint(t0.x), vec2(0.5), 16.0).rgb, lum), 0.02);
        // (softened: a texture's brightest bits are often 3x its average)
        l.albedo = mix(l.albedo, base * pow(dot(l.albedo, lum) / average, 0.6), biomeColor);
    }
    l.albedo *= t1.rgb;
    if (l.roughness < 0.0) l.roughness = t1.w;
    return l;
}

void main()
{
    VeMaterial m = veMaterial();
    vec3 N = normalize(inNormal);
    gDx = dFdx(inWorldPos);
    gDy = dFdy(inWorldPos);
    float dist = distance(inWorldPos, frame.cameraPosition.xyz);

    // Two scales of detail noise: break up the vertex colours, and vary the
    // textured layers on a large scale (no repeats visible from afar).
    float d1 = veSample(m.textures[0], inWorldPos.xz * m.params[0].x).r;
    // The far scale itself repeats every ~20 m, which reads as a grid from a
    // kilometre away: a rotated copy 7.6x larger takes over with distance.
    const mat2 R = mat2(0.6, 0.8, -0.8, 0.6);
    float d2 = mix(veSample(m.textures[0], inWorldPos.xz * m.params[0].y).r,
                   veSample(m.textures[0], R * inWorldPos.xz * (m.params[0].y * 0.131) + 0.5).r,
                   0.35 + 0.65 * smoothstep(100.0, 600.0, dist));
    vec3 base = inColor * mix(0.78, 1.22, d1 * 0.6 + d2 * 0.4);

    // Splat layers (weights below ~1% are skipped), then the vertex colour.
    vec3 albedo = vec3(0.0), bump = vec3(0.0);
    float roughness = 0.0, ao = 0.0;
    float rest = 1.0 - (inSplat.x + inSplat.y + inSplat.z);
    float restW = rest;
    if (rest < 0.999) {
        Layer ls[3];
        float w[3] = float[3](inSplat.x, inSplat.y, inSplat.z);
        // Height blend: a layer's high texels (stones, leaves) win near its borders.
        const float depth = 0.25;
        float top = rest * 0.5 + rest;
        for (int c = 0; c < 3; ++c) {
            uint row = (inLayers >> (8 * c)) & 0xFFu;
            if (row == 0xFFu || w[c] < 0.01) { w[c] = 0.0; continue; }
            ls[c] = layer(m.textures[1], row, row == uint(m.params[2].y), N, base, dist);
            top = max(top, ls[c].height * w[c] + w[c]);
        }
        float sum = max(rest * 0.5 + rest - (top - depth), 0.0);
        restW = sum;
        for (int c = 0; c < 3; ++c) {
            if (w[c] == 0.0) continue;
            float b = max(ls[c].height * w[c] + w[c] - (top - depth), 0.0);
            w[c] = b;
            sum += b;
        }
        float inv = 1.0 / max(sum, 1e-5);
        restW *= inv;
        float macro = mix(0.85, 1.15, d2);
        for (int c = 0; c < 3; ++c) {
            if (w[c] == 0.0) continue;
            float wc = w[c] * inv;
            albedo += ls[c].albedo * macro * wc;
            bump += ls[c].bump * wc;
            roughness += ls[c].roughness * wc;
            ao += ls[c].ao * wc;
        }
    }
    albedo += base * restW;
    roughness += 0.95 * restW;
    ao += mix(0.75, 1.0, d2) * restW;
    N = normalize(N + bump);

    // Wetness: near the waterline, and everywhere when it rains.
    float shore = smoothstep(0.8, 0.0, inWorldPos.y - m.params[2].x);
    float wet = clamp(max(shore, (int(m.params[1].w) >= 0 ? frame.globalParams[int(m.params[1].w)].x : 0.0) * 0.8), 0.0, 1.0);
    albedo *= mix(1.0, 0.55, wet);

    VeSurface s = veDefaultSurface(inWorldPos, N, albedo);
    s.roughness = mix(roughness, 0.35, wet);
    s.ao = ao;
    outColor = vec4(veEvaluateLighting(s), 1.0);
    outGBuffer = veGBuffer(N, s.roughness);
}
