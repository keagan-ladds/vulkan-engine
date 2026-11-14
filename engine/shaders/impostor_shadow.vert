#version 460
// Impostor shadow caster: the quad faces the light (built from the shadow
// view-projection rows), the fragment shader alpha-tests the nearest frame.
#include "ve.glsl"

layout(location = 2) in vec2 inUV;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec2 outUV;
layout(location = 2) out vec3 outObjPos;
layout(location = 3) flat out vec3 outObjDir; // light travel direction, object space

void main()
{
    VeMaterial m = veMaterial();
    mat4 model = veModelMatrix();
    float scale = length(model[0].xyz);
    mat3 rot = mat3(model) / scale;
    vec3 pos = model[3].xyz;
    vec3 centre = (model * vec4(m.params[0].xyz, 1.0)).xyz;
    float radius = m.params[0].w * scale;

    mat4 vp = lights.shadowViews[draw.viewIndex];
    vec3 right = normalize(vec3(vp[0][0], vp[1][0], vp[2][0]));
    vec3 up = normalize(vec3(vp[0][1], vp[1][1], vp[2][1]));
    vec3 forward = normalize(vec3(vp[0][2], vp[1][2], vp[2][2]));
    vec3 world = centre + (right * (inUV.x * 2.0 - 1.0) + up * (1.0 - inUV.y * 2.0)) * radius;

    outWorldPos = world;
    outUV = inUV;
    outObjPos = transpose(rot) * (world - pos) / scale;
    outObjDir = transpose(rot) * forward;
    gl_Position = vp * vec4(world, 1.0);
}
