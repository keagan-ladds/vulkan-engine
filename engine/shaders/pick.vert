#version 460
// Editor picking: object positions only (see ve/editor/PickingPass).
#include "ve.glsl"

layout(location = 0) in vec3 inPosition;

void main()
{
    gl_Position = frame.viewProj * (veModelMatrix() * vec4(inPosition, 1.0));
}
