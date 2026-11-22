#version 460
// Standard mesh vertex shader. Also a good template for custom material
// vertex shaders: same inputs (ve::Vertex), same outputs.
#include "ve.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inTangent;
layout(location = 4) in vec4 inColor;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec2 outUV;
layout(location = 3) out vec4 outTangent;
layout(location = 4) out vec4 outColor;
layout(location = 5) flat out float outLodFade;

void main()
{
    mat4 model = veModelMatrix(); // regular draw or GPU instance
    vec4 world = model * vec4(inPosition, 1.0);
    // Inverse-transpose keeps normals perpendicular under non-uniform scale.
    // The cofactor matrix is the inverse-transpose times det(M), so it needs no
    // division or inverse(); the sign of det keeps mirrored instances' normals
    // pointing out, and the result is normalized below anyway.
    mat3 m = mat3(model);
    mat3 normalMatrix = mat3(cross(m[1], m[2]), cross(m[2], m[0]), cross(m[0], m[1]));
    float handedness = sign(dot(m[0], cross(m[1], m[2])));
    normalMatrix *= handedness;
    outWorldPos = world.xyz;
    outNormal = normalize(normalMatrix * inNormal);
    // A mirroring transform flips cross(N, T), so the bitangent sign flips with it.
    outTangent = vec4(normalize(mat3(model) * inTangent.xyz), inTangent.w * handedness);
    outUV = inUV;
    // Optional per-instance tint (instance custom.w > 0), handy for foliage variation.
    vec4 custom = veInstanceCustom();
    outColor = custom.w > 0.0 ? vec4(inColor.rgb * custom.rgb, inColor.a) : inColor;
    outLodFade = veInstanceLodFade(); // LOD crossfade (instanced draws)
    gl_Position = frame.viewProj * world;
}
