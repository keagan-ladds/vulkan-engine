#version 460
// Icon baking (ve/ui/IconBaker): a mesh with its vertex colours, no scene data.
layout(push_constant) uniform Push {
    mat4 viewProj;
    vec4 sunDir;     // xyz towards which light travels
    vec4 sunColor;
    vec4 sky;
    vec4 ground;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inTangent;
layout(location = 4) in vec4 inColor;

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec4 outColor;

void main()
{
    outNormal = inNormal;
    outColor = inColor;
    gl_Position = pc.viewProj * vec4(inPosition, 1.0);
}
