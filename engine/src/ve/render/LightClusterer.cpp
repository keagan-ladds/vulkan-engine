#include "ve/render/LightClusterer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ve {

// Each light covers the screen tiles of its bounding box's projection in the
// depth slices it spans. If the index list overflows, the clusters are flagged
// invalid and shaders fall back to looping over every light.
void LightClusterer::build(const CameraData& cam, LightBuffer& lb, uint32_t count, bool enabled)
{
    const float z0 = std::max(cam.nearPlane, 0.5f), z1 = std::max(cam.farPlane, z0 * 2.0f);
    const float scale = float(kClusterZ) / std::log(z1 / z0);
    lb.clusterParams = glm::vec4(scale, -std::log(z0) * scale, enabled ? 1.0f : 0.0f, 0.0f);
    m_clusterWords = 0;
    if (!enabled) return;
    if (count == 0) {
        std::memset(lb.clusters, 0, sizeof(lb.clusters));
        return;
    }
    auto slice = [&](float z) { return std::clamp(int(std::floor(std::log(std::max(z, 1e-3f)) * lb.clusterParams.x + lb.clusterParams.y)), 0, int(kClusterZ) - 1); };

    // Per light: its cluster box (tile and slice ranges). Scratch reused every frame.
    using Range = ClusterRange;
    std::vector<Range>& ranges = m_clusterRanges;
    ranges.resize(count);
    std::vector<uint16_t>& perCluster = m_clusterCounts;
    perCluster.assign(kClusterCount, 0);
    for (uint32_t i = 0; i < count; ++i) {
        const glm::vec4 pr = lb.point[i].positionRange;
        const glm::vec3 vc = glm::vec3(cam.view * glm::vec4(glm::vec3(pr), 1.0f));
        const float r = pr.w, depth = -vc.z;
        Range& R = ranges[i];
        R = {0, int(kClusterX) - 1, 0, int(kClusterY) - 1, slice(depth - r), slice(depth + r)};
        if (depth - r > cam.nearPlane) {
            // In front of the camera: the screen rectangle of its view-space box.
            glm::vec2 lo(1e9f), hi(-1e9f);
            for (int c = 0; c < 8; ++c) {
                const glm::vec3 v = vc + glm::vec3(c & 1 ? r : -r, c & 2 ? r : -r, c & 4 ? r : -r);
                const glm::vec4 clip = cam.proj * glm::vec4(v, 1.0f);
                const glm::vec2 uv = glm::vec2(clip) / clip.w * 0.5f + 0.5f;
                lo = glm::min(lo, uv);
                hi = glm::max(hi, uv);
            }
            if (hi.x < 0.0f || hi.y < 0.0f || lo.x > 1.0f || lo.y > 1.0f) {
                R.z1 = R.z0 - 1; // off screen
                continue;
            }
            R.x0 = std::clamp(int(lo.x * kClusterX), 0, int(kClusterX) - 1);
            R.x1 = std::clamp(int(hi.x * kClusterX), 0, int(kClusterX) - 1);
            R.y0 = std::clamp(int(lo.y * kClusterY), 0, int(kClusterY) - 1);
            R.y1 = std::clamp(int(hi.y * kClusterY), 0, int(kClusterY) - 1);
        }
        for (int z = R.z0; z <= R.z1; ++z)
            for (int y = R.y0; y <= R.y1; ++y)
                for (int x = R.x0; x <= R.x1; ++x) ++perCluster[size_t((z * kClusterY + y) * kClusterX + x)];
    }
    // Offsets (prefix sum), then the lists.
    uint32_t total = 0;
    for (uint32_t c = 0; c < kClusterCount; ++c) {
        lb.clusters[c] = total << 9 | perCluster[c];
        total += perCluster[c];
        perCluster[c] = 0;
    }
    if (total > kClusterIndexCapacity) {
        lb.clusterParams.z = 0.0f; // too many: every pixel loops over all lights
        return;
    }
    auto* bytes = reinterpret_cast<uint8_t*>(lb.clusterLights);
    for (uint32_t i = 0; i < count; ++i) {
        const Range& R = ranges[i];
        for (int z = R.z0; z <= R.z1; ++z)
            for (int y = R.y0; y <= R.y1; ++y)
                for (int x = R.x0; x <= R.x1; ++x) {
                    const size_t c = size_t((z * kClusterY + y) * kClusterX + x);
                    bytes[(lb.clusters[c] >> 9) + perCluster[c]++] = uint8_t(i); // little-endian: byte k = bits 8k..8k+7
                }
    }
    m_clusterWords = (total + 3) / 4;
}

} // namespace ve
