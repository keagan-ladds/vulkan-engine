#pragma once

// =============================================================================
// FrameCapture - writes rendered frames to PNG files (part of Renderer).
//
// Two features share the "copy the finished image to a buffer, read it on
// the CPU" path:
//   - Photo capture (Renderer::beginPhotoCapture): the next frames render
//     offscreen at the photo's size with a sub-pixel jittered camera; their
//     linear colour is averaged and written as one anti-aliased PNG.
//   - Headless captures (Renderer::requestCapture, and the env vars
//     VE_CAPTURE=out.png [VE_CAPTURE_FRAME=N, default 150]
//     [VE_CAPTURE_COUNT=n: n frames, out_0.png ... out_<n-1>.png]):
//     the offscreen backbuffer of one frame is saved as is (tests / CI).
//
// The Renderer calls beginFrame() before preparing a frame, recordReadback()
// at the end of its command buffer and endFrame() after submitting it.
// =============================================================================

#include "ve/gfx/Resources.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ve {

class Device;
class Swapchain;
class RenderGraph;

struct PhotoCaptureSettings {
    uint32_t width = 0, height = 0;
    uint32_t samples = 64;
    uint32_t warmupFrames = 12;
    std::string path = "photo.png";
};

class FrameCapture {
public:
    // `headless`: VE_CAPTURE* are read (they only apply to headless runs).
    void init(Device& device, const Swapchain& swapchain, RenderGraph& graph, bool headless);
    void shutdown();

    void requestCapture(const std::string& pngPath) { m_capturePath = pngPath; }

    void beginPhoto(const PhotoCaptureSettings& settings);
    bool photoActive() const { return m_photo.active; }
    float photoProgress() const;
    const std::string& lastPhotoPath() const { return m_photo.lastPath; }
    // A photo is being rendered: frames go to photoTarget() at its size.
    bool photoRendering() const { return m_photo.active && m_photo.started; }
    const Image& photoTarget() const { return m_photo.target; }

    // --- Per frame (Renderer::render) -----------------------------------------------
    // Starts a requested photo (creates its target, resizes the graph).
    // Returns whether this frame renders for the photo.
    bool beginFrame();
    // Photo frames past the warm-up: 1, 2, ... (index of the jitter sample); 0 otherwise.
    uint32_t photoSample() const;
    // After the backbuffer was transitioned to TRANSFER_SRC: copies it to the
    // photo's readback buffer, or to the capture buffer if a headless capture
    // is due this frame (`headlessTarget` = the offscreen backbuffer).
    void recordReadback(VkCommandBuffer cmd, bool headless, const Image& headlessTarget, bool loadingFrame);
    // After submitting with `fence`: waits for it if something was read back,
    // then accumulates the photo sample (may write the photo) or saves the capture.
    void endFrame(VkFence fence, uint64_t frameNumber);

private:
    void photoBegin();
    void photoAccumulate();
    void photoFinish();
    void saveCapture(uint64_t frameNumber); // m_captureBuffer -> m_capturePath

    Device* m_device = nullptr;
    const Swapchain* m_swapchain = nullptr;
    RenderGraph* m_graph = nullptr;

    struct PhotoState {
        bool active = false;
        bool started = false;
        PhotoCaptureSettings settings;
        uint32_t frame = 0;
        Image target;
        Buffer readback;
        std::vector<float> accum; // linear RGB sums
        std::string lastPath;
    };
    PhotoState m_photo;
    bool m_photoFrame = false; // this frame renders for the photo

    // Headless captures.
    const Image* m_captureSource = nullptr; // this frame's image being read back
    Buffer m_captureBuffer;
    std::string m_capturePath; // non-empty: capture the next headless frame
    bool m_capturePending = false; // this frame is read back into m_captureBuffer
    std::string m_envCapturePath;
    uint64_t m_envCaptureFrame = 0; // counted in world frames (not loading screens)
    uint64_t m_envCaptureCount = 1;
    uint64_t m_worldFrames = 0;
};

} // namespace ve
