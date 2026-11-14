// =============================================================================
// ve_lighting.glsl - PBR lighting + shadow sampling for material shaders.
//
//   VeSurface s;  s.position = ...; s.normal = ...; s.albedo = ...; ...
//   vec3 color = veEvaluateLighting(s);
//
// Lighting model: Cook-Torrance GGX specular + Lambert diffuse for every
// directional and point light, with shadows, plus hemispherical ambient
// (frame.ambientSky / ambientGround) and emissive.
// =============================================================================
#ifndef VE_LIGHTING_GLSL
#define VE_LIGHTING_GLSL

#include "ve.glsl"

struct VeSurface {
    vec3 position;  // world
    vec3 normal;    // world, normalized
    vec3 albedo;    // linear
    float roughness;
    float metallic;
    float ao;
    vec3 emissive;  // linear, already multiplied by intensity
};

VeSurface veDefaultSurface(vec3 position, vec3 normal, vec3 albedo)
{
    VeSurface s;
    s.position = position;
    s.normal = normal;
    s.albedo = albedo;
    s.roughness = 0.8;
    s.metallic = 0.0;
    s.ao = 1.0;
    s.emissive = vec3(0.0);
    return s;
}

// ---- Shadows -------------------------------------------------------------------

// Which cascade a point at `viewDepth` falls in (or -1 if beyond the last).
int veCascadeIndex(VeDirectionalLight L, float viewDepth)
{
    for (int c = 0; c < L.shadow.y; ++c) {
        if (viewDepth <= L.cascadeSplits[c]) return c;
    }
    return -1;
}

// Visibility (0 = fully shadowed, 1 = lit) of directional light `i`.
float veDirectionalShadow(uint i, vec3 P, vec3 N, float viewDepth)
{
    VeDirectionalLight L = lights.directional[i];
    if (L.shadow.x < 0) return 1.0;
    int c = veCascadeIndex(L, viewDepth);
    if (c < 0) return 1.0;

    // Normal-offset bias: sample from slightly above the surface, scaled by
    // the size of a shadow texel in this cascade. Kills acne on slopes.
    vec3 offsetP = P + N * L.cascadeTexel[c] * frame.shadowParams.x;
    vec4 sp = lights.shadowViews[L.shadow.x + c] * vec4(offsetP, 1.0);
    vec3 ndc = sp.xyz / sp.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    if (ndc.z >= 1.0) return 1.0;
    float layer = float(L.shadow.z + c);

    // 3x3 PCF; each tap is itself a bilinear 2x2 comparison in hardware.
    vec2 texel = vec2(frame.shadowParams.y / frame.shadowParams.z);
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            sum += texture(veDirShadowMap, vec4(uv + vec2(x, y) * texel, layer, ndc.z));
        }
    }
    float shadow = sum / 9.0;

    // Fade out at the very end of the shadow distance instead of a hard edge.
    float last = L.cascadeSplits[L.shadow.y - 1];
    float fade = clamp((last - viewDepth) / (last * 0.1), 0.0, 1.0);
    return mix(1.0, shadow, fade);
}

// Visibility of point light `i`.
float vePointShadow(uint i, vec3 P, vec3 N)
{
    VePointLight L = lights.point[i];
    if (L.shadow.x < 0) return 1.0;
    vec3 lightPos = L.positionRange.xyz;
    float range = L.positionRange.w;
    float dist = length(P - lightPos);
    // World size of one cube-map texel at this distance (90 degree faces).
    float texelWorld = 2.0 * dist / frame.shadowParams.w;
    vec3 toP = (P + N * texelWorld * 1.5) - lightPos;
    float ref = length(toP) / range - L.params.x;

    // 4 taps around the lookup direction.
    vec3 dir = normalize(toP);
    vec3 up = abs(dir.y) < 0.99 ? vec3(0, 1, 0) : vec3(1, 0, 0);
    vec3 t = normalize(cross(up, dir));
    vec3 b = cross(dir, t);
    float r = 1.5 * texelWorld / max(dist, 1e-3);
    float layer = float(L.shadow.x);
    float sum = texture(vePointShadowMap, vec4(dir + (t + b) * r, layer), ref);
    sum += texture(vePointShadowMap, vec4(dir + (t - b) * r, layer), ref);
    sum += texture(vePointShadowMap, vec4(dir + (-t + b) * r, layer), ref);
    sum += texture(vePointShadowMap, vec4(dir + (-t - b) * r, layer), ref);
    return sum * 0.25;
}

// ---- BRDF ----------------------------------------------------------------------

float veDistributionGGX(float NdotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (VE_PI * d * d);
}

float veGeometrySmith(float NdotV, float NdotL, float roughness)
{
    float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    float gv = NdotV / (NdotV * (1.0 - k) + k);
    float gl = NdotL / (NdotL * (1.0 - k) + k);
    return gv * gl;
}

vec3 veFresnelSchlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

// Radiance reflected towards V from a light arriving from direction L.
vec3 veBRDF(VeSurface s, vec3 V, vec3 L, vec3 radiance)
{
    vec3 N = s.normal;
    vec3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    if (NdotL <= 0.0) return vec3(0.0);
    float NdotV = max(dot(N, V), 1e-4);
    float NdotH = max(dot(N, H), 0.0);
    float rough = clamp(s.roughness, 0.045, 1.0);

    vec3 F0 = mix(vec3(0.04), s.albedo, s.metallic);
    vec3 F = veFresnelSchlick(max(dot(H, V), 0.0), F0);
    float D = veDistributionGGX(NdotH, rough);
    float G = veGeometrySmith(NdotV, NdotL, rough);
    vec3 specular = D * G * F / (4.0 * NdotV * NdotL + 1e-4);
    vec3 kd = (1.0 - F) * (1.0 - s.metallic);
    return (kd * s.albedo / VE_PI + specular) * radiance * NdotL;
}

// Smooth inverse-square falloff that reaches exactly zero at `range`.
float vePointAttenuation(float dist, float range)
{
    float r = dist / range;
    float window = clamp(1.0 - r * r * r * r, 0.0, 1.0);
    return window * window / (dist * dist + 1.0);
}

vec3 veAmbient(VeSurface s, vec3 V)
{
    vec3 N = s.normal;
    vec3 hemi = mix(frame.ambientGround.rgb, frame.ambientSky.rgb, N.y * 0.5 + 0.5);
    vec3 F0 = mix(vec3(0.04), s.albedo, s.metallic);
    float NdotV = max(dot(N, V), 1e-4);
    // Karis' analytic approximation of the split-sum environment BRDF.
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = s.roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    vec2 AB = vec2(-1.04, 1.04) * a004 + r.zw;
    vec3 R = reflect(-V, N);
    vec3 envSpec = mix(frame.ambientGround.rgb, frame.ambientSky.rgb, clamp(R.y * 0.5 + 0.5, 0.0, 1.0));
    vec3 diffuse = hemi * s.albedo * (1.0 - s.metallic);
    vec3 specular = envSpec * (F0 * AB.x + AB.y);
    return (diffuse + specular) * s.ao;
}

vec3 veEvaluateLighting(VeSurface s)
{
    vec3 V = normalize(frame.cameraPosition.xyz - s.position);
    float viewDepth = dot(s.position - frame.cameraPosition.xyz, frame.cameraForward.xyz);
    vec3 color = vec3(0.0);

    for (uint i = 0; i < frame.lightCounts.x; ++i) {
        VeDirectionalLight L = lights.directional[i];
        vec3 l = -L.direction.xyz;
        if (dot(s.normal, l) <= 0.0) continue;
        float shadow = veDirectionalShadow(i, s.position, s.normal, viewDepth);
        color += veBRDF(s, V, l, L.color.rgb * shadow);
    }
    for (uint i = 0; i < frame.lightCounts.y; ++i) {
        VePointLight L = lights.point[i];
        vec3 toLight = L.positionRange.xyz - s.position;
        float dist = length(toLight);
        if (dist >= L.positionRange.w) continue;
        vec3 l = toLight / dist;
        if (dot(s.normal, l) <= 0.0) continue;
        float shadow = vePointShadow(i, s.position, s.normal);
        color += veBRDF(s, V, l, L.color.rgb * vePointAttenuation(dist, L.positionRange.w) * shadow);
    }
    color += veAmbient(s, V) + s.emissive;

    // Debug: tint by cascade of the first shadowed directional light.
    if ((frame.lightCounts.w & 1u) != 0u && frame.lightCounts.x > 0u) {
        int c = veCascadeIndex(lights.directional[0], viewDepth);
        const vec3 tints[4] = vec3[4](vec3(1, 0.3, 0.3), vec3(0.3, 1, 0.3), vec3(0.3, 0.3, 1), vec3(1, 1, 0.3));
        if (c >= 0) color *= tints[c];
    }
    return color;
}

#endif // VE_LIGHTING_GLSL
