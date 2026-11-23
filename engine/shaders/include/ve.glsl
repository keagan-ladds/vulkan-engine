// =============================================================================
// ve.glsl - the shared shader interface. #include "ve.glsl" in every shader.
//
// Declares everything bound by the engine (see docs/01-architecture.md §5.1):
//   frame.*            per-frame globals (camera, time, ambient, globalParams...)
//   lights.*           directional + point lights, shadow view matrices
//   materials[]        all material parameter blocks
//   veDirShadowMap     directional cascades (sampler2DArrayShadow)
//   vePointShadowMap   point light cubes (samplerCubeArrayShadow)
//   veTextures[]       bindless textures (+ veTextures3D[] / veTexturesCube[] aliases)
//   decals.*           this frame's decals + their clusters (functions: ve_decals.glsl)
//   draw.*             default per-draw push constants (unless VE_CUSTOM_PUSH_CONSTANTS)
//
// !!! Keep in sync with engine/src/ve/render/GpuTypes.hpp !!!
// =============================================================================
#ifndef VE_GLSL
#define VE_GLSL

#extension GL_EXT_nonuniform_qualifier : require

#define VE_MAX_DIRECTIONAL_LIGHTS 4
#define VE_MAX_POINT_LIGHTS 256
#define VE_MAX_GRASS_BENDERS 8
#define VE_MAX_SHADOW_VIEWS 128
#define VE_CLUSTER_X 16
#define VE_CLUSTER_Y 9
#define VE_CLUSTER_Z 24
#define VE_CLUSTER_INDEX_CAPACITY 65536
#define VE_MAX_DECALS 1024
#define VE_DECAL_INDEX_CAPACITY 32768
#define VE_INVALID 0xFFFFFFFFu
#define VE_DRAW_FLAG_POINT_SHADOW 1u
#define VE_DRAW_FLAG_INSTANCED 2u
#define VE_PI 3.14159265359

layout(set = 0, binding = 0, std140) uniform VeFrameGlobals {
    mat4 view;
    mat4 proj;
    mat4 viewProj;
    mat4 invView;
    mat4 invProj;
    mat4 invViewProj;
    mat4 prevViewProj;
    vec4 cameraPosition;   // xyz, w = near
    vec4 cameraForward;    // xyz, w = far
    vec4 screenSize;       // w, h, 1/w, 1/h
    vec4 time;             // time, dt, frame, realTime
    vec4 ambientSky;
    vec4 ambientGround;    // rgb, w = sunlit-bounce fraction (scaled by sun visibility: veAmbientGround)
    uvec4 lightCounts;     // dir, point, cascades, debug flags
    vec4 shadowParams;     // normal bias (texels), pcf radius (texels), dir res, point res
    uvec4 builtinTextures; // SceneDepth, SceneColor, SceneNormal
    vec4 globalParams[8];
    vec4 jitter;           // xy = how far the sub-pixel camera jitter moved the image, in UV units (TAA, photo mode)
    vec4 grassBenders[VE_MAX_GRASS_BENDERS]; // xyz = position (feet), w = radius (0 = unused)
} frame;

struct VeDirectionalLight {
    vec4 direction;     // direction the light travels
    vec4 color;         // rgb * intensity
    ivec4 shadow;       // first view, cascade count, first layer
    vec4 cascadeSplits; // view depth at the end of each cascade
    vec4 cascadeTexel;  // world size of a shadow texel per cascade
};

struct VePointLight {
    vec4 positionRange;
    vec4 color;
    ivec4 shadow;       // cube index (-1 none), first view
    vec4 params;        // x = shadow bias
};

layout(set = 0, binding = 1, std430) readonly buffer VeLightBuffer {
    VeDirectionalLight directional[VE_MAX_DIRECTIONAL_LIGHTS];
    VePointLight point[VE_MAX_POINT_LIGHTS];
    mat4 shadowViews[VE_MAX_SHADOW_VIEWS];
    vec4 clusterParams;  // depth slice = log(view depth) * x + y; z = 1 if valid
    uint clusters[VE_CLUSTER_X * VE_CLUSTER_Y * VE_CLUSTER_Z]; // first << 9 | count
    uint clusterLights[VE_CLUSTER_INDEX_CAPACITY / 4];         // 8-bit point light indices
} lights;

struct VeMaterial {
    vec4 params[12];
    uint textures[16];
};

layout(set = 0, binding = 2, std430) readonly buffer VeMaterialBuffer {
    VeMaterial materials[];
};

// Decals: oriented boxes projecting colour / normal / roughness / metallic
// onto surfaces (ve::DecalProjector, ve::DecalSystem). Binned into the light
// cluster grid; material shaders apply them with veApplyDecals (ve_decals.glsl).
struct VeDecal {
    vec4 rows[3];   // world -> decal space (box -0.5..0.5), row-major affine
    vec4 color;     // rgb linear colour, a = opacity
    vec4 opacity;   // per channel: albedo, normal, roughness, metallic
    vec4 params;    // roughness, metallic, normal strength, flatten
    vec4 fade;      // cos(angle fade start), cos(angle fade end), depth fade, blend (0 normal, 1 multiply)
    uvec4 textures; // albedo, normal, mask (G rough, B metal, A opacity), layer mask
};

layout(set = 0, binding = 14, std430) readonly buffer VeDecalBuffer {
    uvec4 info;         // x = count, y = 1 if clusters are valid
    vec4 clusterParams; // depth slice = log(view depth) * x + y
    uint clusters[VE_CLUSTER_X * VE_CLUSTER_Y * VE_CLUSTER_Z]; // first << 16 | count
    VeDecal list[VE_MAX_DECALS];
    uint clusterDecals[VE_DECAL_INDEX_CAPACITY / 2];           // 16-bit decal indices
} decals;

layout(set = 0, binding = 3) uniform sampler2DArrayShadow veDirShadowMap;
layout(set = 0, binding = 4) uniform samplerCubeArrayShadow vePointShadowMap;

// Bindless textures. The same descriptor array is visible through several
// sampler types; use the one that matches the texture you stored.
layout(set = 1, binding = 1) uniform sampler2D veTextures[];
layout(set = 1, binding = 1) uniform sampler3D veTextures3D[];
layout(set = 1, binding = 1) uniform samplerCube veTexturesCube[];

// 4x4 ordered (Bayer) dither threshold for pixel p, in (0, 1): every 4x4
// block holds all 16 values (k + 0.5) / 16. Arithmetic, no lookup table:
// the index is the bit-interleave of (x ^ y) and y, bits reversed.
float veBayer4(ivec2 p)
{
    uint x = uint(p.x) & 3u, y = uint(p.y) & 3u, b = x ^ y;
    uint v = ((b & 1u) << 3) | ((y & 1u) << 2) | (b & 2u) | ((y & 2u) >> 1);
    return (float(v) + 0.5) / 16.0;
}

#ifndef VE_CUSTOM_PUSH_CONSTANTS
layout(push_constant) uniform VeDrawPush {
    mat4 model;
    uint materialIndex;
    uint viewIndex;
    uint lightIndex;
    uint flags;
    vec4 custom;
} draw;

VeMaterial veMaterial() { return materials[draw.materialIndex]; }

#ifdef VE_VERTEX_STAGE
// ---- GPU instancing ------------------------------------------------------------
// Instanced draws (InstanceGroup) get their transform from the instance
// buffer: the cull shader wrote the visible instance indices, and
// gl_InstanceIndex (offset by the indirect command's firstInstance) picks one.
// Use veModelMatrix() instead of draw.model in every mesh vertex shader so it
// works for both regular and instanced draws.
struct VeInstance {
    vec4 rows[3];
    vec4 custom;
};
layout(set = 0, binding = 5, std430) readonly buffer VeInstanceBuffer { VeInstance veInstances[]; };
layout(set = 0, binding = 6, std430) readonly buffer VeVisibleBuffer { uint veVisibleInstances[]; };

bool veIsInstanced() { return (draw.flags & VE_DRAW_FLAG_INSTANCED) != 0u; }

// Visible entries pack the instance index (bits 0-22) with LOD crossfade data.
uint veVisibleEntry() { return veVisibleInstances[gl_InstanceIndex]; }
uint veInstanceIndex() { return veVisibleEntry() & 0x7FFFFFu; }

mat4 veModelMatrix()
{
    if (!veIsInstanced()) return draw.model;
    VeInstance inst = veInstances[veInstanceIndex()];
    return transpose(mat4(inst.rows[0], inst.rows[1], inst.rows[2], vec4(0.0, 0.0, 0.0, 1.0)));
}

// Per-instance custom data (InstanceGroup::add(..., custom)); zero for regular draws.
vec4 veInstanceCustom()
{
    return veIsInstanced() ? veInstances[veInstanceIndex()].custom : vec4(0.0);
}

// LOD crossfade value for this instance copy: 0 = fully visible,
// > 0 = incoming LOD (keeps a `fade` fraction of pixels), < 0 = outgoing LOD.
// Pass it to the fragment shader in a `flat` varying and call veLodDither().
float veInstanceLodFade()
{
    if (!veIsInstanced()) return 0.0;
    uint e = veVisibleEntry();
    uint q = (e >> 23) & 0xFFu;
    if (q == 0u) return 0.0;
    float f = float(q) / 255.0;
    return (e >> 31) != 0u ? f : -f;
}
#endif // VE_VERTEX_STAGE

#ifdef VE_FRAGMENT_STAGE
// ---- Thin G-buffer --------------------------------------------------------------
// Opaque mesh shaders render to two targets: location 0 = lit colour,
// location 1 = SceneNormal. Declare
//     layout(location = 1) out vec4 outGBuffer;
// and write  outGBuffer = veGBuffer(worldNormal, roughness);
// Screen-space effects (SSR, SSGI) read it. Shaders that don't write it leave
// undefined values there (those pixels then look wrong in SSR/SSGI).
vec4 veGBuffer(vec3 worldNormal, float roughness)
{
    return vec4(normalize(worldNormal), clamp(roughness, 0.02, 1.0));
}

// LOD crossfade: the outgoing and incoming LOD copies discard complementary
// pixels of a 4x4 ordered dither, so together they cover the surface exactly
// once while the mix shifts smoothly from one LOD to the other.
void veLodDither(float fade)
{
    if (fade == 0.0) return;
    float d = veBayer4(ivec2(gl_FragCoord.xy));
    if (fade > 0.0 ? d >= fade : d < -fade) discard;
}
#endif
#endif

// ---- Helpers -----------------------------------------------------------------

vec4 veSample(uint index, vec2 uv) { return texture(veTextures[nonuniformEXT(index)], uv); }
vec4 veSampleLod(uint index, vec2 uv, float lod) { return textureLod(veTextures[nonuniformEXT(index)], uv, lod); }
vec4 veSampleGrad(uint index, vec2 uv, vec2 dx, vec2 dy) { return textureGrad(veTextures[nonuniformEXT(index)], uv, dx, dy); }
vec4 veSample3D(uint index, vec3 uvw) { return texture(veTextures3D[nonuniformEXT(index)], uvw); }
vec4 veSample3DLod(uint index, vec3 uvw, float lod) { return textureLod(veTextures3D[nonuniformEXT(index)], uvw, lod); }

// Tangent-space normal from a normal-map sample. Uses only X and Y and
// rebuilds Z: cooked normal maps are BC5 (two channels, B reads 0), and for
// uncooked RGB maps it gives the same result.
vec3 veUnpackNormal(vec4 texel)
{
    vec2 xy = texel.xy * 2.0 - 1.0;
    return vec3(xy, sqrt(clamp(1.0 - dot(xy, xy), 0.0, 1.0)));
}

// Samples `index` if valid, otherwise returns `fallback`.
vec4 veSampleOr(uint index, vec2 uv, vec4 fallback)
{
    return index == VE_INVALID ? fallback : veSample(index, uv);
}

#ifdef VE_FRAGMENT_STAGE
// StandardMaterial alpha test (cutout shadow casters): discards when
// params[0].a * albedo alpha is below the params[1].w cutoff (0 = off);
// albedo UVs are scaled by params[3].xy.
void veStandardCutout(VeMaterial m, vec2 uv)
{
    float alpha = m.params[0].a * veSampleOr(m.textures[0], uv * m.params[3].xy, vec4(1.0)).a;
    if (m.params[1].w > 0.0 && alpha < m.params[1].w) discard;
}
#endif

// World position from a screen UV (0..1, top-left origin) and a depth buffer value.
vec3 veWorldFromDepth(vec2 uv, float depth)
{
    vec4 clip = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 world = frame.invViewProj * clip;
    return world.xyz / world.w;
}

// Distance along the view direction for a (reversed-Z) depth value.
float veLinearDepth(float depth)
{
    vec4 v = frame.invProj * vec4(0.0, 0.0, depth, 1.0);
    return -v.z / v.w;
}

// World-space view ray through a screen UV.
vec3 veViewRay(vec2 uv)
{
    return normalize(veWorldFromDepth(uv, 0.5) - frame.cameraPosition.xyz);
}

float veLuminance(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// Cheap per-pixel noise in [0,1) (Jimenez interleaved gradient noise), handy to dither raymarches.
float veInterleavedGradientNoise(vec2 pixel)
{
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

// Hash functions (Dave Hoskins).
float veHash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
float veHash13(vec3 p3)
{
    p3 = fract(p3 * 0.1031);
    p3 += dot(p3, p3.zyx + 31.32);
    return fract((p3.x + p3.y) * p3.z);
}

// Smooth value noise.
float veValueNoise(vec3 p)
{
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = veHash13(i), n100 = veHash13(i + vec3(1, 0, 0));
    float n010 = veHash13(i + vec3(0, 1, 0)), n110 = veHash13(i + vec3(1, 1, 0));
    float n001 = veHash13(i + vec3(0, 0, 1)), n101 = veHash13(i + vec3(1, 0, 1));
    float n011 = veHash13(i + vec3(0, 1, 1)), n111 = veHash13(i + vec3(1, 1, 1));
    return mix(mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y),
               mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y), f.z);
}

#endif // VE_GLSL
