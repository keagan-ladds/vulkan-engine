#version 460
// Octahedral impostor: a camera-facing quad covering the object's bounding
// sphere. The fragment shader reconstructs the object from the atlas.
// Material: params[0] = object-space centre (xyz) + radius (w),
//           params[1].x = frames per side, params[1].y = alpha cutoff, params[1].z = roughness,
//           textures[0] = albedo atlas, textures[1] = normal atlas.
#include "ve.glsl"

layout(location = 2) in vec2 inUV;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outObjPos;          // this pixel's point on the quad, object space
layout(location = 2) flat out vec3 outObjCamera;  // camera position, object space
layout(location = 3) flat out mat3 outRotation;   // object -> world rotation (normals)
layout(location = 6) flat out vec4 outTint;
layout(location = 7) flat out float outLodFade;

void main()
{
    VeMaterial m = veMaterial();
    mat4 model = veModelMatrix();
    float scale = length(model[0].xyz);
    mat3 rot = mat3(model) / scale;
    vec3 pos = model[3].xyz;
    vec3 centre = (model * vec4(m.params[0].xyz, 1.0)).xyz;
    float radius = m.params[0].w * scale;

    // Camera right/up from the view matrix.
    vec3 right = vec3(frame.view[0][0], frame.view[1][0], frame.view[2][0]);
    vec3 up = vec3(frame.view[0][1], frame.view[1][1], frame.view[2][1]);
    vec3 world = centre + (right * (inUV.x * 2.0 - 1.0) + up * (1.0 - inUV.y * 2.0)) * radius;

    outWorldPos = world;
    outObjPos = transpose(rot) * (world - pos) / scale;
    outObjCamera = transpose(rot) * (frame.cameraPosition.xyz - pos) / scale;
    outRotation = rot;
    outTint = veInstanceCustom();
    outLodFade = veInstanceLodFade();
    gl_Position = frame.viewProj * vec4(world, 1.0);
}
