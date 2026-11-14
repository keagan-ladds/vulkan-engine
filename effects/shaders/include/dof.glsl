// Shared by dof_gather.frag / dof_apply.frag (DepthOfFieldPass).
#ifndef DOF_GLSL
#define DOF_GLSL

layout(push_constant) uniform Push {
    vec4 params;  // x focus distance (m, if not auto), y in-focus range (m), z max blur (pixels), w auto focus (0/1)
    vec4 params2; // x taps, y near blur scale, z aperture, w unused
    uint depthTex;
    uint colorTex;
    uint blurTex;  // dof_gather/dof_apply: the buffer to read
} pc;

float dofDepth(vec2 uv)
{
    float d = veSampleLod(pc.depthTex, uv, 0.0).r;
    return d <= 0.0 ? 6e4 : min(veLinearDepth(d), 6e4); // fits in fp16
}

// Auto focus: the closest of a few samples around the screen centre.
float dofFocusDistance()
{
    if (pc.params.w < 0.5) return pc.params.x;
    float f = dofDepth(vec2(0.5));
    f = min(f, dofDepth(vec2(0.48, 0.5)));
    f = min(f, dofDepth(vec2(0.52, 0.5)));
    f = min(f, dofDepth(vec2(0.5, 0.47)));
    f = min(f, dofDepth(vec2(0.5, 0.53)));
    return min(f, 5000.0);
}

// Circle of confusion in 0..1 (multiply by the max blur radius). Thin-lens
// model: blur grows with |1/focus - 1/depth|, so focusing far away keeps the
// mid-ground sharp while close focus blurs the background strongly. Depths
// within focusRange/2 of the focus distance count as in focus.
float dofCoc(float depth, float focus)
{
    float halfRange = pc.params.y * 0.5;
    float d = depth > focus ? max(focus, depth - halfRange) : min(focus, depth + halfRange);
    float coc = pc.params2.z * abs(1.0 / focus - 1.0 / max(d, 0.01));
    return clamp(depth < focus ? coc * pc.params2.y : coc, 0.0, 1.0);
}

#endif
