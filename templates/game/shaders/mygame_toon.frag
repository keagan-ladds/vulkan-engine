#version 460
// Example custom fragment shader: toon (cel) shading instead of the engine's
// PBR. Paired with the engine's standard.vert, so the inputs below are what
// that vertex shader outputs.
//
// Material layout (set from C++ with material->setVector(slot, ...)):
//   params[0] = base colour (rgba)
//   params[1].x = number of light bands
//
// Name game shaders with a prefix (mygame_) so they can't clash with the
// engine's shader file names.
#include "ve_lighting.glsl" // ve.glsl + shadow helpers (veDirectionalShadow, vePointShadow...)

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inTangent;
layout(location = 4) in vec4 inColor;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outGBuffer; // thin G-buffer for screen-space effects (see ve.glsl)

void main()
{
    VeMaterial m = veMaterial();
    vec3 albedo = m.params[0].rgb;
    float bands = max(m.params[1].x, 1.0);
    vec3 N = normalize(inNormal);
    vec3 V = normalize(frame.cameraPosition.xyz - inWorldPos);
    float viewDepth = dot(inWorldPos - frame.cameraPosition.xyz, frame.cameraForward.xyz);

    // Ambient from the engine's sky/ground colours. veAmbientGround() removes the
    // sunlit-ground bounce where the sun is hidden (e.g. behind hills at sunset).
    VeSurface surface = veDefaultSurface(inWorldPos, N, albedo);
    vec3 color = albedo * mix(veAmbientGround(surface), frame.ambientSky.rgb, N.y * 0.5 + 0.5);

    // Directional lights: quantise N.L into flat bands (with shadows).
    for (uint i = 0u; i < frame.lightCounts.x; ++i) {
        VeDirectionalLight L = lights.directional[i];
        float ndl = max(dot(N, -L.direction.xyz), 0.0) * veDirectionalShadow(i, inWorldPos, N, viewDepth);
        float banded = floor(ndl * bands + 0.5) / bands;
        color += albedo * L.color.rgb * banded;
    }
    // Point lights: same idea with the engine's falloff, only those whose
    // range reaches this pixel's light cluster.
    VeLightList list = veLightList(inWorldPos, viewDepth);
    for (uint k = 0u; k < list.count; ++k) {
        VePointLight L = lights.point[veLightAt(list, k)];
        vec3 toLight = L.positionRange.xyz - inWorldPos;
        float d = length(toLight);
        if (d >= L.positionRange.w) continue;
        // (veSpotFactor: the cone of a SpotLight; 1 for point lights.)
        float ndl = max(dot(N, toLight / d), 0.0) * vePointAttenuation(d, L.positionRange.w) * veSpotFactor(L, toLight / d);
        color += albedo * L.color.rgb * floor(ndl * bands * 4.0 + 0.5) / (bands * 4.0);
    }

    // Dark rim for a drawn look.
    float rim = 1.0 - max(dot(N, V), 0.0);
    color *= mix(1.0, 0.35, smoothstep(0.75, 0.95, rim));
    outColor = vec4(color, 1.0);
    outGBuffer = veGBuffer(N, 0.9);
}
