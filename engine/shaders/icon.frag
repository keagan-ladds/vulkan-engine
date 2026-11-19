#version 460
layout(push_constant) uniform Push {
    mat4 viewProj;
    vec4 sunDir;
    vec4 sunColor;
    vec4 sky;
    vec4 ground;
} pc;

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec4 inColor;
layout(location = 0) out vec4 outColor;

void main()
{
    vec3 n = normalize(inNormal);
    if (!gl_FrontFacing) n = -n;
    float sun = max(dot(n, -normalize(pc.sunDir.xyz)), 0.0);
    vec3 ambient = mix(pc.ground.rgb, pc.sky.rgb, n.y * 0.5 + 0.5);
    vec3 albedo = inColor.rgb * pc.sunColor.a; // sunColor.a carries the tint's brightness
    vec3 c = albedo * (ambient + pc.sunColor.rgb * sun * 1.15);
    outColor = vec4(c, 1.0); // written to an sRGB image: linear here
}
