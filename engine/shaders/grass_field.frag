#version 460
// Default GrassPass fragment shader: lit as if the normal pointed up (like the
// ground, keeps a field soft instead of noisy), darker at the roots, with
// back-lit translucency from the first directional light.
#include "ve_lighting.glsl"

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec4 inColor;
layout(location = 2) in float inTip;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outGBuffer;

void main()
{
    vec3 N = vec3(0.0, 1.0, 0.0);
    VeSurface s = veDefaultSurface(inWorldPos, N, inColor.rgb);
    s.roughness = 0.9;
    s.ao = mix(0.75, 1.0, inTip);
    vec3 color = veEvaluateLighting(s);
    if (frame.lightCounts.x > 0u) {
        VeDirectionalLight L = lights.directional[0];
        vec3 V = normalize(frame.cameraPosition.xyz - inWorldPos);
        float back = pow(clamp(dot(-V, -L.direction.xyz), 0.0, 1.0), 6.0);
        float viewDepth = dot(inWorldPos - frame.cameraPosition.xyz, frame.cameraForward.xyz);
        color += L.color.rgb * back * inColor.rgb * inTip * 0.12 * veDirectionalShadow(0u, inWorldPos, N, viewDepth);
    }
    outColor = vec4(color, 1.0);
    outGBuffer = veGBuffer(N, 0.9);
}
