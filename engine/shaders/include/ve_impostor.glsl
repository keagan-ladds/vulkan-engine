// =============================================================================
// ve_impostor.glsl - shared math for octahedral impostors (baking + rendering).
//
// An impostor atlas holds N x N "frames": pictures of the object taken from
// directions spread over the upper hemisphere. Frame (i, j) looks FROM
// direction hemiOctDecode((i, j) / (N - 1)) towards the object's centre with an
// orthographic camera that exactly frames its bounding sphere.
// !!! Must match Impostor.cpp (the C++ baker uses the same conventions). !!!
// =============================================================================
#ifndef VE_IMPOSTOR_GLSL
#define VE_IMPOSTOR_GLSL

// Hemi-octahedral mapping: unit direction with y >= 0  <->  [0,1]^2.
vec2 veHemiOctEncode(vec3 d)
{
    d /= abs(d.x) + abs(d.y) + abs(d.z);
    return vec2(d.x + d.z, d.x - d.z) * 0.5 + 0.5;
}

vec3 veHemiOctDecode(vec2 uv)
{
    vec2 f = uv * 2.0 - 1.0;
    vec3 d = vec3((f.x + f.y) * 0.5, 0.0, (f.x - f.y) * 0.5);
    d.y = 1.0 - abs(d.x) - abs(d.z);
    return normalize(d);
}

// Camera basis of the frame that looks from direction `d` (right, up).
void veImpostorBasis(vec3 d, out vec3 right, out vec3 up)
{
    right = abs(d.y) < 0.999 ? normalize(cross(vec3(0.0, 1.0, 0.0), d)) : vec3(1.0, 0.0, 0.0);
    up = cross(d, right);
}

// Atlas UV where the ray (o + t * dir) hits frame `frame` (integer grid coords),
// in object space; `inside` is false if it misses the frame. `frameUV` is the
// unclamped position within the frame (0..1 inside): it varies smoothly across
// the screen, so its derivatives / N are the right texture gradients (the
// atlas UV jumps at frame borders).
vec2 veImpostorFrameUV(ivec2 frame, float N, vec3 o, vec3 dir, vec3 centre, float radius, out bool inside,
                       out vec2 frameUV)
{
    vec3 d = veHemiOctDecode(vec2(frame) / (N - 1.0));
    vec3 right, up;
    veImpostorBasis(d, right, up);
    float denom = dot(dir, d);
    float t = abs(denom) > 1e-5 ? dot(centre - o, d) / denom : 0.0;
    vec3 local = o + dir * t - centre;
    vec2 uv = 0.5 + 0.5 * vec2(dot(local, right), dot(local, up)) / radius;
    inside = all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)));
    frameUV = uv;
    return (vec2(frame) + clamp(uv, 0.0, 1.0)) / N;
}

vec2 veImpostorFrameUV(ivec2 frame, float N, vec3 o, vec3 dir, vec3 centre, float radius, out bool inside)
{
    vec2 frameUV;
    return veImpostorFrameUV(frame, N, o, dir, centre, radius, inside, frameUV);
}

// The three frames around view direction `v` (object space, y clamped >= 0)
// with barycentric weights.
void veImpostorFrames(vec3 v, float N, out ivec2 f0, out ivec2 f1, out ivec2 f2, out vec3 w)
{
    vec2 g = veHemiOctEncode(v) * (N - 1.0);
    vec2 base = floor(g);
    vec2 t = g - base;
    ivec2 b = ivec2(base);
    int maxIndex = int(N) - 1;
    if (t.x + t.y < 1.0) {
        f0 = b;
        f1 = b + ivec2(1, 0);
        f2 = b + ivec2(0, 1);
        w = vec3(1.0 - t.x - t.y, t.x, t.y);
    } else {
        f0 = b + ivec2(1, 1);
        f1 = b + ivec2(1, 0);
        f2 = b + ivec2(0, 1);
        w = vec3(t.x + t.y - 1.0, 1.0 - t.y, 1.0 - t.x);
    }
    f0 = clamp(f0, ivec2(0), ivec2(maxIndex));
    f1 = clamp(f1, ivec2(0), ivec2(maxIndex));
    f2 = clamp(f2, ivec2(0), ivec2(maxIndex));
}

#endif // VE_IMPOSTOR_GLSL
