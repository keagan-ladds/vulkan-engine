#version 460
// Distant mountains: lit like any surface (vertex-coloured, rough), then
// hazed towards the sky's own colour in that direction with distance and
// less with height (aerial perspective), so ranges separate into layers.
// params[0] = (haze density per m, height falloff per m, max haze, -); textures[0] = SkyViewLUT.
#include "ve_lighting.glsl"
#include "vfx.glsl"

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inTangent;
layout(location = 4) in vec4 inColor;
layout(location = 5) flat in float inLodFade;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outGBuffer;

void main()
{
    VeMaterial m = veMaterial();
    VeSurface s;
    s.position = inWorldPos;
    s.normal = normalize(inNormal);
    s.albedo = inColor.rgb;
    s.roughness = 0.92;
    s.metallic = 0.0;
    s.ao = 1.0;
    s.emissive = vec3(0.0);
    vec3 lit = veEvaluateLighting(s);

    vec3 toP = inWorldPos - frame.cameraPosition.xyz;
    float dist = length(toP);
    vec3 dir = toP / dist;
    vec3 sky = m.textures[0] != VE_INVALID ? vfxSampleSky(m.textures[0], normalize(vec3(dir.x, max(dir.y, 0.03), dir.z)), VFX_SUN.xyz)
                                           : frame.ambientSky.rgb;
    // Optical depth through air thinning with height (average height of the path).
    float midHeight = max(0.5 * (frame.cameraPosition.y + inWorldPos.y), 0.0);
    float optical = m.params[0].x * dist * exp(-midHeight * m.params[0].y);
    float haze = min(1.0 - exp(-optical), m.params[0].z);
    outColor = vec4(mix(lit, sky, haze), 1.0);
    outGBuffer = veGBuffer(s.normal, s.roughness);
}
