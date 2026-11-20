#pragma once

// =============================================================================
// ShadowSetup - where this frame's shadow maps look from (part of Renderer).
//
// Pure math on the camera and the light parameters, no GPU work: fits the
// cascades of a directional light around the camera frustum and builds the
// six cube-face views of a shadowed point light. Each shadow view's matrix
// goes into LightBuffer::shadowViews (for the shaders) and a ShadowView into
// FrameData::shadowViews (for ShadowPass / instance culling).
// =============================================================================

#include "ve/render/FrameData.hpp"

namespace ve {

struct RendererSettings;

class ShadowSetup {
public:
    // `settings` is read live (the F1 overlay changes it); `depthClamp` is
    // whether the device clamps depth in directional shadow pipelines.
    void init(const RendererSettings& settings, bool depthClamp)
    {
        m_settings = &settings;
        m_depthClamp = depthClamp;
    }

    // The direction directional shadow maps are rendered from (snapped to
    // RendererSettings::shadowDirectionStep; lighting keeps the exact one).
    glm::vec3 shadowDirection(const glm::vec3& lightDirection) const;

    // Fits settings.cascadeCount cascades of directional light `lightIndex`
    // (shadow slot `shadowSlot`) to the camera in `frame` and fills `gpu`'s
    // cascade fields. `sceneTop` = highest drawn geometry this frame (bounds
    // how far towards the sun casters are gathered).
    void buildCascades(FrameData& frame, float sceneTop, const glm::vec3& lightDir, uint32_t lightIndex,
                       uint32_t shadowSlot, GpuDirectionalLight& gpu) const;

    // The six cube-face views of point light `lightIndex` (position and range
    // from gpu.positionRange) in cube map `cube`; sets gpu.shadow.
    void buildPointFaces(FrameData& frame, uint32_t lightIndex, uint32_t cube, GpuPointLight& gpu) const;

private:
    const RendererSettings* m_settings = nullptr;
    bool m_depthClamp = false;
};

} // namespace ve
