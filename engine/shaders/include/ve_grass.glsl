// =============================================================================
// ve_grass.glsl - clump placement for GrassPass (vertex stage).
//
// Material layout written by GrassPass:
//   params[0] = boundsMin.xy, 1/boundsSize.xy      textures[0] = heights (R32F, exact samples)
//   params[1] = colorA.rgb, spacing                textures[1] = mask (x density, y colour mix, z height scale, w free)
//   params[2] = colorB.rgb, radius
//   params[3] = minHeight, maxHeight, bladeWidth, fadeStart
//   params[4] = farDensity, windSlot (-1 none), sway, maxTilt (radians)
//   params[5] = mode (0 blades, 1 mesh/cards), alphaCutoff  textures[2] = card / mesh texture
//   params[6] = this LOD's camera distance range (min, max), dither width
// draw.custom = tile origin x/z, tile size, clumps per full tile.
//
// Blade mode, the clump mesh: per blade 3 vertices; tangent.xyz = blade root in
// the clump, uv.x = per-blade random, uv.y = 0 root .. 1 tip, color.r = height factor.
// Mesh mode (cards or authored grass): any mesh, root at the origin, height 1,
// scaled uniformly by the clump height; vertex colour multiplies the clump colour.
// LODs: GrassPass draws each LOD with its own material. A clump belongs to the
// LOD whose distance range holds its (per-clump jittered) distance, so the
// switch is spread over params[6].z metres instead of a hard ring.
// =============================================================================
#ifndef VE_GRASS_GLSL
#define VE_GRASS_GLSL

#include "ve.glsl"

struct VeGrassClump {
    vec3 root;    // world position of the clump on the ground
    float height; // blade height (0 = hidden)
    float width;
    float yaw;
    vec3 tiltAxis; // horizontal
    float tilt;    // radians
    float fade;   // 0..1: grows in/out at the density threshold
    float distance;
    vec4 mask;
    vec3 color;
    bool visible;
};

// Ground height from exact samples (texelFetch: R32F needn't be filterable),
// interpolated over the same two triangles per cell as the terrain mesh
// (split along the (i, j+1) -> (i+1, j) diagonal), so roots sit on the
// rendered surface instead of a bilinear one that floats or sinks mid-cell.
float veGrassGroundHeight(VeMaterial m, vec2 xz)
{
    vec2 res = vec2(textureSize(veTextures[nonuniformEXT(m.textures[0])], 0));
    vec2 t = clamp((xz - m.params[0].xy) * m.params[0].zw, 0.0, 1.0) * (res - 1.0);
    ivec2 i = min(ivec2(floor(t)), ivec2(res) - 2);
    vec2 f = t - vec2(i);
    float h00 = texelFetch(veTextures[nonuniformEXT(m.textures[0])], i, 0).r;
    float h10 = texelFetch(veTextures[nonuniformEXT(m.textures[0])], i + ivec2(1, 0), 0).r;
    float h01 = texelFetch(veTextures[nonuniformEXT(m.textures[0])], i + ivec2(0, 1), 0).r;
    float h11 = texelFetch(veTextures[nonuniformEXT(m.textures[0])], i + ivec2(1, 1), 0).r;
    if (f.x + f.y < 1.0) return h00 + (h10 - h00) * f.x + (h01 - h00) * f.y;
    return h11 + (h01 - h11) * (1.0 - f.x) + (h10 - h11) * (1.0 - f.y);
}

VeGrassClump veGrassClump(VeMaterial m)
{
    VeGrassClump c;
    vec4 tile = draw.custom;
    float i = float(gl_InstanceIndex);
    // R2 low-discrepancy sequence, offset per tile: any prefix is evenly spread.
    vec2 offset = vec2(veHash12(tile.xy * 0.173 + 3.1), veHash12(tile.xy * 0.311 + 7.7));
    vec2 xz = tile.xy + fract(offset + i * vec2(0.7548776662, 0.5698402910)) * tile.z;

    vec2 uv = (xz - m.params[0].xy) * m.params[0].zw;
    c.mask = veSampleLod(m.textures[1], uv, 0.0);
    float radius = m.params[2].w;
    float fadeStart = m.params[3].w;
    c.distance = length(xz - frame.cameraPosition.xz);
    float t = clamp((c.distance - fadeStart) / max(radius - fadeStart, 1.0), 0.0, 1.0);
    float keep = mix(1.0, m.params[4].x, smoothstep(0.0, 1.0, t)) * (1.0 - smoothstep(radius * 0.85, radius, c.distance));

    // Keep this clump if its index fraction is under the wanted density.
    float margin = c.mask.x * keep - (i + 0.5) / tile.w;
    c.fade = clamp(margin / 0.02, 0.0, 1.0);
    c.visible = margin > 0.0 && all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)));
    float lodDistance = c.distance + (veHash12(xz * 4.37 + 2.9) - 0.5) * m.params[6].z;
    c.visible = c.visible && lodDistance >= m.params[6].x && lodDistance < m.params[6].y;

    float r = veHash12(xz * 13.7 + 0.31);
    c.height = mix(m.params[3].x, m.params[3].y, r) * c.mask.z * mix(0.4, 1.0, c.fade);
    c.width = m.params[3].z * mix(1.0, 2.5, t); // far blades get wider to keep coverage as they thin out
    c.yaw = veHash12(xz * 7.31 + 1.7) * 6.2831853;
    float ta = veHash12(xz * 5.17 + 9.3) * 6.2831853;
    c.tiltAxis = vec3(cos(ta), 0.0, sin(ta));
    c.tilt = veHash12(xz * 2.71 + 4.2) * m.params[4].w;
    c.width *= mix(0.8, 1.25, veHash12(xz * 8.9 + 2.2));
    c.root = vec3(xz.x, veGrassGroundHeight(m, xz) - 0.03, xz.y);
    c.color = mix(m.params[1].rgb, m.params[2].rgb, c.mask.y) * (0.85 + 0.3 * veHash12(xz * 3.1 + 5.0));
    return c;
}

// Rotates a clump-local offset by the clump's yaw, then its tilt.
vec3 veGrassRotate(VeGrassClump c, vec3 off)
{
    float s = sin(c.yaw), co = cos(c.yaw);
    off.xz = vec2(co * off.x - s * off.z, s * off.x + co * off.z);
    // Rodrigues rotation around the horizontal tilt axis.
    vec3 k = c.tiltAxis;
    float ct = cos(c.tilt), st = sin(c.tilt);
    return off * ct + cross(k, off) * st + k * dot(k, off) * (1.0 - ct);
}

// Card mode: uniform scale by the clump height, then rotate.
vec3 veGrassCardPosition(VeGrassClump c, vec3 position)
{
    return c.root + veGrassRotate(c, position * c.height);
}

// Blade mode: world position of a clump-mesh vertex (before wind).
vec3 veGrassVertexPosition(VeGrassClump c, vec3 position, vec3 bladeRoot, vec2 uv, float heightFactor)
{
    vec3 local = position - bladeRoot;
    float h = c.height * heightFactor;
    vec3 off = vec3(local.x, local.y * h, local.z);
    off.xz *= uv.y > 0.5 ? h : c.width * 2.0; // root: blade width; tip: lean scales with height
    off += bladeRoot * vec3(1.0, 0.0, 1.0);
    return c.root + veGrassRotate(c, off);
}

// Full placement of a clump-mesh vertex: blades or cards (params[5].x), then
// wind from frame.globalParams[windSlot]. Used by the colour and shadow
// vertex shaders, so shadows match exactly. tip = sway weight (0 root .. 1 tip).
vec3 veGrassPlace(VeMaterial m, VeGrassClump c, vec3 position, vec4 tangent, vec2 uv, vec4 color, out float tip)
{
    bool cards = m.params[5].x > 0.5;
    vec3 world = cards ? veGrassCardPosition(c, position) : veGrassVertexPosition(c, position, tangent.xyz, uv, color.r);
    // Mesh clumps can be wider than a terrain cell: shift each vertex by the
    // ground under it (relative to the root) so the whole clump follows the
    // slope instead of floating on the downhill side and sinking uphill.
    if (cards) world.y += veGrassGroundHeight(m, world.xz) - (c.root.y + 0.03);
    tip = cards ? clamp(position.y, 0.0, 1.0) : uv.y;
    float phase = cards ? veHash12(c.root.xz) : uv.x;

    int windSlot = int(m.params[4].y);
    if (windSlot >= 0) {
        vec4 wind = frame.globalParams[windSlot];
        float w = tip * tip * m.params[4].z;
        float t = frame.time.x;
        // Rolling gusts across the field + per-blade flutter.
        float wave = sin(dot(world.xz, wind.xy) * 0.15 - t * 1.6) * 0.5 + 0.5;
        float flutter = sin(t * 5.0 + phase * 30.0 + world.x * 0.7) * 0.25;
        float bend = ((0.25 + wave * 0.75) * wind.z * 0.045 + flutter * 0.05) * c.height;
        world.xz += wind.xy * bend * w;
        world.y -= abs(bend) * w * 0.3;
    }
    return world;
}

#endif
