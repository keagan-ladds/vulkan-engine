#version 460
// Water (transparent queue). Shows several engine features at once:
//  - reads SceneDepth (frame.builtinTextures.x) to know how deep the water is
//    -> clear shallows, murky depths, foam on the shore
//  - samples the atmosphere's SkyViewLUT through the bindless table
//    (material textures[0]) for sky reflections
//  - reacts to rain (VFX_WEATHER.y) with ripples
// Material: params[0] deep colour, params[1] shallow colour,
//           params[2] = absorption, foam, ripple scale, ripple strength.
#include "ve_lighting.glsl"
#include "vfx.glsl"

layout(location = 0) in vec3 inWorldPos;
layout(location = 0) out vec4 outColor;

float waveHeight(vec2 p)
{
    float t = frame.time.x;
    vec2 w = VFX_WIND.xy * t * 0.4;
    return veValueNoise(vec3(p * 0.35 + w, t * 0.3)) * 0.6 + veValueNoise(vec3(p * 1.1 - w * 1.7, t * 0.5)) * 0.4;
}

void main()
{
    VeMaterial m = veMaterial();
    vec3 V = normalize(frame.cameraPosition.xyz - inWorldPos);

    // Normal from the slope of a procedural height field.
    float scale = m.params[2].z;
    vec2 p = inWorldPos.xz * scale;
    float e = 0.05;
    float h0 = waveHeight(p);
    vec2 grad = vec2(waveHeight(p + vec2(e, 0)) - h0, waveHeight(p + vec2(0, e)) - h0) / e;
    float rain = VFX_WEATHER.y;
    if (rain > 0.01) {
        float rp = veValueNoise(vec3(inWorldPos.xz * 6.0, frame.time.x * 8.0));
        grad += (vec2(dFdx(rp), dFdy(rp)) * 40.0) * rain;
    }
    float strength = m.params[2].w * (1.0 + rain * 2.0);
    vec3 N = normalize(vec3(-grad.x * strength, 1.0, -grad.y * strength));

    // Water depth along the view ray: scene behind the surface minus surface.
    vec2 screenUV = gl_FragCoord.xy * frame.screenSize.zw;
    float sceneDist = veLinearDepth(veSampleLod(frame.builtinTextures.x, screenUV, 0.0).r);
    float surfaceDist = veLinearDepth(gl_FragCoord.z);
    float depth = max(sceneDist - surfaceDist, 0.0);

    // Body colour: shallow -> deep with distance travelled through the water.
    float absorb = 1.0 - exp(-depth * m.params[2].x);
    vec3 body = mix(m.params[1].rgb, m.params[0].rgb, absorb);
    VeSurface s = veDefaultSurface(inWorldPos, vec3(0, 1, 0), body);
    s.roughness = 1.0;
    vec3 lit = veEvaluateLighting(s);

    // Reflection of the sky + a sharp sun glint.
    float fresnel = min(0.02 + 0.98 * pow(1.0 - max(dot(N, V), 0.0), 5.0), 0.65);
    vec3 R = reflect(-V, N);
    R.y = abs(R.y);
    vec3 sky = m.textures[0] != VE_INVALID ? vfxSampleSky(m.textures[0], R, VFX_SUN.xyz) : frame.ambientSky.rgb;
    float glint = pow(max(dot(R, VFX_SUN.xyz), 0.0), 900.0) * 60.0;
    vec3 reflection = sky * 0.55 + VFX_SUN_COLOR.rgb * glint * vfxSunShadow(inWorldPos);

    // Foam where the water is very shallow.
    float foam = smoothstep(0.25, 0.0, depth) * m.params[2].y * (0.6 + 0.4 * h0);
    vec3 color = mix(lit, reflection, fresnel) + foam * frame.ambientSky.rgb * 0.8;
    float alpha = clamp(absorb * 0.85 + fresnel + foam, 0.0, 1.0);
    alpha *= smoothstep(0.0, 0.08, depth); // soft edge where water meets land
    outColor = vec4(color, alpha);
}
