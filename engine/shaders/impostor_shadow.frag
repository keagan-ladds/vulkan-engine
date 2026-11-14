#version 460
#include "ve.glsl"
#include "ve_impostor.glsl"

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec3 inObjPos;
layout(location = 3) flat in vec3 inObjDir;

void main()
{
    VeMaterial m = veMaterial();
    vec3 centre = m.params[0].xyz;
    float radius = m.params[0].w;
    float N = m.params[1].x;
    vec3 v = -inObjDir; // towards the light
    v.y = max(v.y, 0.0);
    v = length(v) > 1e-5 ? normalize(v) : vec3(0.0, 1.0, 0.0);
    ivec2 f0, f1, f2;
    vec3 w;
    veImpostorFrames(v, N, f0, f1, f2, w);
    ivec2 f = w.x >= w.y && w.x >= w.z ? f0 : (w.y >= w.z ? f1 : f2); // nearest frame is enough for shadows
    bool inside;
    vec2 uv = veImpostorFrameUV(f, N, inObjPos - inObjDir * radius * 4.0, inObjDir, centre, radius, inside);
    if (!inside || veSampleLod(m.textures[0], uv, 0.0).a < m.params[1].y) discard;
}
