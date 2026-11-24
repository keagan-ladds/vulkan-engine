#pragma once

// =============================================================================
// TemporalAccumulator - accumulates a noisy screen-space buffer over frames
// (used by ScreenSpaceGIPass and ScreenSpaceReflectionsPass).
//
// Owns two ping-ponged history buffers (+ linear depth for rejection). Each
// frame: reproject the history with frame.prevViewProj, reject it where the
// surface changed, clip it to the current 3x3 neighbourhood (mean +- gamma
// sigma) and blend in the new frame. The noisy pass should vary its sampling
// pattern every frame (e.g. by frame.time.z), or there is nothing new to add.
//
//   setup():   m_accum.setup(renderer(), graph, "SSR", 0.5f);
//   declare(): m_accum.declare(builder);
//   execute(): TextureId smooth = m_accum.resolve(ctx, noisyBuffer);  // Sampled on return
// =============================================================================

#include <ve/render/PipelineHandle.hpp>
#include <ve/render/RenderGraph.hpp>

#include <string>

namespace vfx {

/// Accumulates a noisy screen-space result over frames, reprojected with the camera (SSR, SSGI).
class TemporalAccumulator {
public:
    ~TemporalAccumulator();
    /// Creates its history textures (`name` prefix, at `scale` of the screen).
    void setup(ve::Renderer& renderer, ve::RenderGraph& graph, const std::string& name, float scale);
    /// Declares its texture uses for this frame.
    void declare(ve::PassBuilder& builder);
    /// Returns the accumulated texture (in Access::Sampled), or `current` if disabled.
    ve::TextureId resolve(ve::RenderContext& ctx, ve::TextureId current);
    /// Drops the history (after a cut or resize).
    void invalidate() { m_valid = false; }

    bool enabled = true;  ///< Off: no accumulation (the raw result is used).
    float blend = 0.1f;     ///< weight of the new frame (~1/blend frames of history)
    float clipGamma = 1.5f; ///< neighbourhood clip width in sigmas (smaller = less ghosting, more noise)

private:
    ve::Renderer* m_renderer = nullptr;
    ve::TextureId m_history[2] = {ve::kNoTexture, ve::kNoTexture};
    ve::TextureId m_depth[2] = {ve::kNoTexture, ve::kNoTexture};
    ve::PipelineHandle m_pipeline;
    uint32_t m_frame = 0;
    bool m_valid = false;
};

} // namespace vfx
