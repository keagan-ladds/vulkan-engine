#version 460
// Editor picking: writes the draw's id (DrawPush::viewIndex) as a float.
#include "ve.glsl"

layout(location = 0) out float outId;

void main()
{
    outId = float(draw.viewIndex);
}
