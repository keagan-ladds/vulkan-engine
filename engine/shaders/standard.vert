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

void main()
{
    mat4 model = veModelMatrix(); // regular draw or GPU instance
    vec4 world = model * vec4(inPosition, 1.0);
    // Inverse-transpose keeps normals perpendicular under non-uniform scale.
    mat3 normalMatrix = transpose(inverse(mat3(model)));
    outWorldPos = world.xyz;
    outNormal = normalize(normalMatrix * inNormal);
    outTangent = vec4(normalize(mat3(model) * inTangent.xyz), inTangent.w);
    outUV = inUV;
    // Optional per-instance tint (instance custom.w > 0), handy for foliage variation.
    vec4 custom = veInstanceCustom();
    outColor = custom.w > 0.0 ? vec4(inColor.rgb * custom.rgb, inColor.a) : inColor;
    gl_Position = frame.viewProj * world;
}
