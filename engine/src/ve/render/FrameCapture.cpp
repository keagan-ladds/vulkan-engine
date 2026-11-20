#include "ve/render/FrameCapture.hpp"

#include "ve/core/Log.hpp"
#include "ve/gfx/Device.hpp"
#include "ve/gfx/Swapchain.hpp"
#include "ve/render/RenderGraph.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace ve {

namespace {

// Copies the whole (TRANSFER_SRC) colour image into a buffer.
void copyToBuffer(VkCommandBuffer cmd, const Image& image, const Buffer& buffer)
{
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = image.extent;
    vkCmdCopyImageToBuffer(cmd, image.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer.handle, 1, &region);
}

} // namespace

void FrameCapture::init(Device& device, const Swapchain& swapchain, RenderGraph& graph, bool headless)
{
    m_device = &device;
    m_swapchain = &swapchain;
    m_graph = &graph;
    if (!headless) return;
    if (const char* c = std::getenv("VE_CAPTURE"); c && *c) {
        m_envCapturePath = c;
        const char* f = std::getenv("VE_CAPTURE_FRAME");
        m_envCaptureFrame = f && *f ? std::strtoull(f, nullptr, 10) : 150;
        // VE_CAPTURE_COUNT=n: n consecutive frames, saved as name_0.png ... name_<n-1>.png
        const char* n = std::getenv("VE_CAPTURE_COUNT");
        m_envCaptureCount = n && *n ? std::max<uint64_t>(1, std::strtoull(n, nullptr, 10)) : 1;
    }
}

void FrameCapture::shutdown()
{
    if (!m_device) return;
    if (m_captureBuffer.handle) m_device->destroyBuffer(m_captureBuffer);
    if (m_photo.target.handle) m_device->destroyImage(m_photo.target);
    if (m_photo.readback.handle) m_device->destroyBuffer(m_photo.readback);
}

// -----------------------------------------------------------------------------
// Per frame
// -----------------------------------------------------------------------------

bool FrameCapture::beginFrame()
{
    m_photoFrame = m_photo.active;
    if (m_photoFrame && !m_photo.started) photoBegin();
    return m_photoFrame;
}

uint32_t FrameCapture::photoSample() const
{
    if (!m_photoFrame || m_photo.frame < m_photo.settings.warmupFrames) return 0;
    return m_photo.frame - m_photo.settings.warmupFrames + 1;
}

void FrameCapture::recordReadback(VkCommandBuffer cmd, bool headless, const Image& headlessTarget, bool loadingFrame)
{
    if (m_photoFrame) copyToBuffer(cmd, m_photo.target, m_photo.readback);
    // VE_CAPTURE_FRAME counts world frames (loading-screen frames don't count).
    const uint64_t worldFrame = m_worldFrames;
    if (!loadingFrame) ++m_worldFrames;
    if (headless && !loadingFrame && !m_envCapturePath.empty() && worldFrame >= m_envCaptureFrame &&
        worldFrame < m_envCaptureFrame + m_envCaptureCount) {
        m_capturePath = m_envCapturePath;
        if (m_envCaptureCount > 1) {
            const std::filesystem::path p(m_envCapturePath);
            m_capturePath = (p.parent_path() / (p.stem().string() + "_" + std::to_string(worldFrame - m_envCaptureFrame) +
                                                p.extension().string())).string();
        }
    }
    m_capturePending = headless && !m_photoFrame && !m_capturePath.empty();
    if (m_capturePending) {
        const VkExtent3D e = headlessTarget.extent;
        const VkDeviceSize bytes = VkDeviceSize(e.width) * e.height * 4;
        if (m_captureBuffer.size < bytes) {
            if (m_captureBuffer.handle) m_device->destroyBuffer(m_captureBuffer);
            m_captureBuffer = m_device->createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::GpuToCpu,
                                                     "capture readback");
        }
        m_captureSource = &headlessTarget;
        copyToBuffer(cmd, headlessTarget, m_captureBuffer);
    }
}

void FrameCapture::endFrame(VkFence fence, uint64_t frameNumber)
{
    VkDevice dev = m_device->handle();
    if (m_photoFrame) {
        VK_CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
        photoAccumulate(); // may finish the photo
    }
    if (m_capturePending) {
        VK_CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
        saveCapture(frameNumber);
        m_capturePending = false;
    }
}

// -----------------------------------------------------------------------------
// Photo capture
// -----------------------------------------------------------------------------

void FrameCapture::beginPhoto(const PhotoCaptureSettings& settings)
{
    if (m_photo.active) return;
    m_photo.settings = settings;
    m_photo.settings.samples = std::max(settings.samples, 1u);
    m_photo.active = true;
    m_photo.started = false;
}

float FrameCapture::photoProgress() const
{
    if (!m_photo.active) return 0.0f;
    const float total = float(m_photo.settings.warmupFrames + m_photo.settings.samples);
    return std::min(float(m_photo.frame) / total, 1.0f);
}

void FrameCapture::photoBegin()
{
    PhotoCaptureSettings& s = m_photo.settings;
    const VkExtent2D win = m_swapchain->extent();
    const uint32_t limit = m_device->properties().limits.maxImageDimension2D;
    const uint32_t w = std::clamp(s.width ? s.width : win.width, 16u, limit);
    const uint32_t h = std::clamp(s.height ? s.height : win.height, 16u, limit);
    m_device->waitIdle();
    m_photo.target = m_device->createImage({.format = m_swapchain->format(),
                                            .extent = {w, h, 1},
                                            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                            .debugName = "photo target"});
    m_photo.readback = m_device->createBuffer(VkDeviceSize(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                              MemoryUsage::GpuToCpu, "photo readback");
    m_photo.accum.assign(size_t(w) * h * 3, 0.0f);
    m_photo.frame = 0;
    m_photo.started = true;
    m_graph->resize({w, h}); // screen-sized textures follow the photo's size
    log::info("Renderer: photo {}x{}, {} samples (+{} warm-up frames)", w, h, s.samples, s.warmupFrames);
}

void FrameCapture::photoAccumulate()
{
    const uint32_t frame = m_photo.frame++;
    if (frame < m_photo.settings.warmupFrames) return;
    vmaInvalidateAllocation(m_device->allocator(), m_photo.readback.allocation, 0, VK_WHOLE_SIZE);
    // sRGB bytes -> linear, so the average is physically right (edges, bright highlights).
    static const std::array<float, 256> toLinear = [] {
        std::array<float, 256> t{};
        for (int i = 0; i < 256; ++i) {
            const float c = float(i) / 255.0f;
            t[size_t(i)] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        return t;
    }();
    const VkFormat f = m_photo.target.format;
    const bool bgr = f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_B8G8R8A8_UNORM;
    const auto* px = static_cast<const uint8_t*>(m_photo.readback.mapped);
    float* acc = m_photo.accum.data();
    const size_t n = m_photo.accum.size() / 3;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* p = px + i * 4;
        acc[i * 3 + 0] += toLinear[bgr ? p[2] : p[0]];
        acc[i * 3 + 1] += toLinear[p[1]];
        acc[i * 3 + 2] += toLinear[bgr ? p[0] : p[2]];
    }
    if (frame + 1 >= m_photo.settings.warmupFrames + m_photo.settings.samples) photoFinish();
}

void FrameCapture::photoFinish()
{
    const VkExtent3D e = m_photo.target.extent;
    const float inv = 1.0f / float(m_photo.settings.samples);
    std::vector<uint8_t> out(size_t(e.width) * e.height * 3);
    for (size_t i = 0; i < out.size(); ++i) {
        const float c = std::clamp(m_photo.accum[i] * inv, 0.0f, 1.0f);
        const float srgb = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
        out[i] = static_cast<uint8_t>(std::lround(srgb * 255.0f));
    }
    const std::filesystem::path path(m_photo.settings.path);
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    if (stbi_write_png(path.string().c_str(), int(e.width), int(e.height), 3, out.data(), int(e.width) * 3)) {
        m_photo.lastPath = std::filesystem::absolute(path, ec).string();
        log::info("Renderer: photo saved to {}", m_photo.lastPath);
    } else {
        log::error("Renderer: failed to write {}", path.string());
    }
    m_device->waitIdle();
    m_device->destroyImage(m_photo.target);
    m_device->destroyBuffer(m_photo.readback);
    m_photo.accum = {};
    m_photo.active = m_photo.started = false;
    m_graph->resize(m_swapchain->extent());
}

// -----------------------------------------------------------------------------
// Headless capture
// -----------------------------------------------------------------------------

void FrameCapture::saveCapture(uint64_t frameNumber)
{
    vmaInvalidateAllocation(m_device->allocator(), m_captureBuffer.allocation, 0, VK_WHOLE_SIZE);
    const VkExtent3D e = m_captureSource->extent;
    std::vector<uint8_t> px(size_t(e.width) * e.height * 4);
    std::memcpy(px.data(), m_captureBuffer.mapped, px.size());
    const VkFormat f = m_captureSource->format;
    if (f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_B8G8R8A8_UNORM)
        for (size_t i = 0; i < px.size(); i += 4) std::swap(px[i], px[i + 2]);
    for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    if (stbi_write_png(m_capturePath.c_str(), int(e.width), int(e.height), 4, px.data(), int(e.width) * 4))
        log::info("Renderer: captured frame {} to {}", frameNumber, m_capturePath);
    else
        log::error("Renderer: failed to write {}", m_capturePath);
    m_capturePath.clear();
}

} // namespace ve
