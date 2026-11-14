#version 460
// Full-screen triangle without any vertex buffer: vertices 0,1,2 become
// (-1,-1), (3,-1), (-1,3), which covers the whole screen. outUV is (0,0) at
// the top-left corner. Depth is 0 = the far plane with reversed-Z, so passes
// can depth-test against SceneDepth to touch only "sky" pixels.

layout(location = 0) out vec2 outUV;

void main()
{
    outUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(outUV * 2.0 - 1.0, 0.0, 1.0);
}
