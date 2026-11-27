#include "ve/render/ShadowSetup.hpp"
#include "ve/render/Renderer.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace ve {

// The direction directional shadow maps are rendered from: the light's,
// snapped to shadowDirectionStep-degree steps of azimuth and elevation. A sun
// that moves every frame would otherwise re-rasterise every shadow edge from a
// slightly different angle each frame, and the edges' stair steps crawl
// (the whole scene seems to shimmer). Snapped, shadows stand still and move in
// small steps; lighting keeps the exact direction.
glm::vec3 ShadowSetup::shadowDirection(const glm::vec3& dir) const
{
    const float step = glm::radians(m_settings->shadowDirectionStep);
    if (step <= 0.0f) return dir;
    float elevation = std::asin(std::clamp(dir.y, -1.0f, 1.0f));
    if (std::abs(dir.y) > 0.9995f) return dir; // overhead: azimuth is meaningless
    float azimuth = std::atan2(dir.z, dir.x);
    elevation = std::round(elevation / step) * step;
    azimuth = std::round(azimuth / step) * step;
    return {std::cos(elevation) * std::cos(azimuth), std::sin(elevation), std::cos(elevation) * std::sin(azimuth)};
}

void ShadowSetup::buildCascades(FrameData& frame, float sceneTop, const glm::vec3& lightDir, uint32_t lightIndex,
                                uint32_t slot, GpuDirectionalLight& gpu) const
{
    const RendererSettings& settings = *m_settings;
    const CameraData& cam = frame.camera;
    const uint32_t n = settings.cascadeCount;
    const float res = static_cast<float>(settings.directionalShadowResolution);
    const float nearP = cam.nearPlane;
    const float farP = std::min(cam.farPlane, settings.maxShadowDistance);
    const float lambda = settings.cascadeSplitLambda;
    const float casterMargin = 150.0f; // how far "behind" the cascade casters are captured

    const uint32_t baseView = slot * kMaxCascades;
    const uint32_t baseLayer = slot * n;
    const glm::vec3 up = std::abs(lightDir.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
    const float tanHalf = std::tan(cam.fovY * 0.5f);

    float prevSplit = nearP;
    float prevBegin = 0.0f; // where the previous cascade's range starts (0 for the first, like the shader)
    for (uint32_t c = 0; c < n; ++c) {
        // "Practical split scheme": blend of logarithmic and uniform splits.
        const float p = static_cast<float>(c + 1) / static_cast<float>(n);
        const float logSplit = nearP * std::pow(farP / nearP, p);
        const float uniSplit = nearP + (farP - nearP) * p;
        const float split = lambda * logSplit + (1.0f - lambda) * uniSplit;

        // Corners of this slice of the camera frustum, in world space.
        glm::vec3 corners[8];
        int k = 0;
        // The shader blends the last 15% of each cascade into the next one, so
        // the next one must cover that band too (16%: a little slack).
        const float sliceStart = c == 0 ? nearP : std::max(nearP, prevSplit - (prevSplit - prevBegin) * 0.16f);
        for (float d : {sliceStart, split}) {
            const float h = tanHalf * d, w = h * cam.aspect;
            const glm::vec3 center = cam.position + cam.forward * d;
            for (float sx : {-1.0f, 1.0f}) {
                for (float sy : {-1.0f, 1.0f}) corners[k++] = center + cam.right * (w * sx) + cam.up * (h * sy);
            }
        }
        glm::vec3 centroid(0.0f);
        for (const auto& v : corners) centroid += v;
        centroid /= 8.0f;
        // A bounding *sphere* keeps the cascade size constant as the camera
        // rotates, which (with texel snapping below) stops shadow edges shimmering.
        float radius = 0.0f;
        for (const auto& v : corners) radius = std::max(radius, glm::length(v - centroid));
        radius = std::ceil(radius * 16.0f) / 16.0f;

        const glm::vec3 eye = centroid - lightDir * (radius + casterMargin);
        const glm::mat4 view = glm::lookAt(eye, centroid, up);
        glm::mat4 proj = glm::ortho(-radius, radius, -radius, radius, 0.0f, 2.0f * radius + casterMargin);

        // Texel snapping: move the projection so world origin lands exactly on a texel.
        glm::vec4 origin = proj * view * glm::vec4(0, 0, 0, 1);
        origin *= res * 0.5f;
        const glm::vec2 rounded = glm::round(glm::vec2(origin));
        const glm::vec2 offset = (rounded - glm::vec2(origin)) * (2.0f / res);
        proj[3][0] += offset.x;
        proj[3][1] += offset.y;

        const glm::mat4 vp = proj * view;
        const uint32_t viewIndex = baseView + c;
        frame.lights->shadowViews[viewIndex] = vp;
        ShadowView sv;
        sv.viewProj = vp;
        sv.frustum = Frustum::fromMatrix(vp);
        // Directional shadow pipelines use depth clamp, so casters between the
        // sun and the cascade (beyond casterMargin, e.g. a distant mountain at
        // sunset) still land in the map at depth 0. Don't cull them by the near
        // plane.
        if (m_depthClamp) {
            // ...but only as far as a caster could still shadow this cascade:
            // going towards the sun, the ray from a receiver climbs sin(elevation)
            // per metre, so nothing further than (scene top - cascade bottom) /
            // sin(elevation) can reach down into it. With a low sun over a big
            // world this skips everything beyond the hills that matter.
            const float sinElevation = -lightDir.y;
            const float top = (sceneTop > -FLT_MAX ? sceneTop : centroid.y) + settings.shadowInstanceHeight;
            const float rise = std::max(top - (centroid.y - radius), 0.0f);
            // Near sunset that bound grows without limit, and everything towards
            // the sun is pancaked into every cascade (overdraw across whole
            // maps); shadowCasterReach caps it.
            float reach = std::max(settings.shadowCasterReach, casterMargin);
            if (sinElevation > 0.02f) reach = std::min(reach, std::max(rise / sinElevation, casterMargin));
            const glm::vec3 p0 = centroid - lightDir * (radius + reach);
            sv.frustum.planes[4] = glm::vec4(lightDir, -glm::dot(lightDir, p0));
        }
        sv.viewIndex = viewIndex;
        sv.layer = baseLayer + c;
        sv.lightIndex = lightIndex;
        sv.texelSize = 2.0f * radius / res;
        frame.shadowViews.push_back(sv);

        gpu.cascadeSplits[c] = split;
        gpu.cascadeTexel[c] = 2.0f * radius / res;
        prevBegin = c == 0 ? 0.0f : prevSplit;
        prevSplit = split;
    }
    gpu.shadow = glm::ivec4(static_cast<int>(baseView), static_cast<int>(n), static_cast<int>(baseLayer), 0);
}

void ShadowSetup::buildPointFaces(FrameData& frame, uint32_t lightIndex, uint32_t cube, GpuPointLight& gpu) const
{
    const glm::vec3 pos = glm::vec3(gpu.positionRange);
    const float range = gpu.positionRange.w;
    // Point light cube faces follow the directional cascades in LightBuffer::shadowViews.
    const uint32_t pointViewBase = kMaxDirectionalLights * kMaxCascades;
    const uint32_t firstView = pointViewBase + cube * 6;
    gpu.shadow = glm::ivec4(static_cast<int>(cube), static_cast<int>(firstView), 0, 0);
    // Standard cube-map face orientation (same as OpenGL; it also works
    // in Vulkan because both the projection and the lookup flip together).
    static const glm::vec3 dirs6[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    static const glm::vec3 ups6[6] = {{0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};
    const glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.05f, range);
    // Spot lights: only the faces the cone reaches (a face spans 45 degrees
    // from its axis to an edge, ~55 to a corner).
    const bool spot = gpu.spot.w > -1.5f;
    const float coneAngle = spot ? std::acos(std::clamp(gpu.spot.w, -1.0f, 1.0f)) : 0.0f;
    for (uint32_t f = 0; f < 6; ++f) {
        if (spot) {
            const float axisAngle = std::acos(std::clamp(glm::dot(glm::vec3(gpu.spot), dirs6[f]), -1.0f, 1.0f));
            if (axisAngle > coneAngle + glm::radians(55.0f)) continue; // the cone can't reach this face
        }
        const glm::mat4 vp = proj * glm::lookAt(pos, pos + dirs6[f], ups6[f]);
        frame.lights->shadowViews[firstView + f] = vp;
        ShadowView sv;
        sv.viewProj = vp;
        sv.frustum = Frustum::fromMatrix(vp);
        sv.viewIndex = firstView + f;
        sv.layer = cube * 6 + f;
        sv.lightIndex = lightIndex;
        sv.point = true;
        sv.lightPosition = pos;
        sv.range = range;
        frame.shadowViews.push_back(sv);
    }
}

} // namespace ve
