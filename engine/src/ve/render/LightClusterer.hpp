#pragma once

// =============================================================================
// LightClusterer - bins this frame's point lights into the camera's clusters
// (part of Renderer).
//
// Fills LightBuffer::clusterParams / clusters / clusterLights so each pixel
// only evaluates the lights that reach it (see ve.glsl). Done on the CPU:
// there are at most a few hundred point lights, already frustum culled.
// The scratch buffers are kept between frames, so binning doesn't allocate.
// =============================================================================

#include "ve/render/FrameData.hpp"

#include <cstdint>
#include <vector>

namespace ve {

class LightClusterer {
public:
    // Bins lights.point[0, pointLights). `enabled` = RendererSettings::clusteredLights
    // (off: the clusters are flagged invalid and shaders loop over every light).
    void build(const CameraData& camera, LightBuffer& lights, uint32_t pointLights, bool enabled);
    // Used words of LightBuffer::clusterLights (only that much is uploaded).
    uint32_t usedWords() const { return m_clusterWords; }

private:
    struct ClusterRange {
        int x0, x1, y0, y1, z0, z1;
    };
    uint32_t m_clusterWords = 0;
    std::vector<ClusterRange> m_clusterRanges; // per light: its cluster box
    std::vector<uint16_t> m_clusterCounts;     // per cluster: lights in it
};

} // namespace ve
