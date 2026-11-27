#version 460
// Icon baking with materials (ve/ui/IconBaker: IconPart): a mesh drawn with its
// StandardMaterial's base colour / texture, lit by a sun and a sky / ground ambient.
#define VE_CUSTOM_PUSH_CONSTANTS
#include "ve.glsl"

layout(push_constant) uniform Push {
    mat4 mvp;       // the part's transform x the icon's view-projection
    vec4 rotation;  // the part's rotation (quaternion: normals)
    vec4 sunDir;    // xyz towards which light travels; w = material index (bits)
    vec4 sunColor;
    vec4 sky;       // rgb; w = ground brightness relative to the sky
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inTangent;
layout(location = 4) in vec4 inColor;

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec2 outUV;
layout(location = 2) out vec4 outColor;

vec3 rotate(vec4 q, vec3 v)
{
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

void main()
{
    outNormal = rotate(pc.rotation, inNormal);
    outUV = inUV;
    outColor = inColor;
    gl_Position = pc.mvp * vec4(inPosition, 1.0);
}
