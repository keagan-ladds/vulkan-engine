#version 460
// GrassPass fragment shader for alpha-clipped grass cards: texture (textures[2])
// x clump colour, discard below the alpha cutoff (params[5].y). Lit like
// grass_field.frag (normal up, back-lit translucency).
#include "ve_lighting.glsl"

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec4 inColor;
layout(location = 2) in float inTip;
layout(location = 3) in vec2 inUV;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outGBuffer;

void main()
{
    VeMaterial m = veMaterial();
    vec4 tex = veSampleOr(m.textures[2], inUV, vec4(1.0));
    // Sharpen alpha with distance-independent derivatives so thin blades
    // survive minification instead of fading out in the mip chain.
    float alpha = (tex.a - m.params[5].y) / max(fwidth(tex.a), 1e-4) + 0.5;
    if (alpha < 0.5) discard;

    vec3 N = vec3(0.0, 1.0, 0.0);
    VeSurface s = veDefaultSurface(inWorldPos, N, tex.rgb * inColor.rgb);
    s.roughness = 0.9;
    s.ao = mix(0.7, 1.0, inTip);
    vec3 color = veEvaluateLighting(s);
    if (frame.lightCounts.x > 0u) {
        VeDirectionalLight L = lights.directional[0];
        vec3 V = normalize(frame.cameraPosition.xyz - inWorldPos);
        float back = pow(clamp(dot(-V, -L.direction.xyz), 0.0, 1.0), 6.0);
        float viewDepth = dot(inWorldPos - frame.cameraPosition.xyz, frame.cameraForward.xyz);
        color += L.color.rgb * back * s.albedo * inTip * 0.12 * veDirectionalShadow(0u, inWorldPos, N, viewDepth);
    }
    outColor = vec4(color, 1.0);
    outGBuffer = veGBuffer(N, 0.9);
}
