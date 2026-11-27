#include "ve/ui/IconBaker.hpp"

#include "ve/gfx/Pipeline.hpp"
#include "ve/gfx/Vk.hpp"
#include "ve/gfx/Device.hpp"
#include "ve/render/Material.hpp"
#include "ve/render/Renderer.hpp"
#include "ve/render/Texture.hpp"
#include "ve/scene/Components.hpp"
#include "ve/scene/GameObject.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <bit>
#include <functional>
#include <cmath>

namespace ve::ui {

namespace {
struct IconPush {
    glm::mat4 viewProj;
    glm::vec4 sunDir, sunColor, sky, ground;
};
static_assert(sizeof(IconPush) <= kPushConstantBytes);

struct MaterialIconPush {
    glm::mat4 mvp;
    glm::vec4 rotation; // quaternion (x, y, z, w)
    glm::vec4 sunDir;   // w = material index (bits)
    glm::vec4 sunColor;
    glm::vec4 sky;      // w = ground / sky brightness
};
static_assert(sizeof(MaterialIconPush) <= kPushConstantBytes);
} // namespace

std::shared_ptr<Texture> bakeIcon(Renderer& r, const MeshData& mesh, uint32_t size, const IconView& v)
{
    if (mesh.vertices.empty() || mesh.indices.empty()) return nullptr;
    Device& dev = r.device();
    const uint32_t big = size * 2; // supersampled, then filtered down
    constexpr VkFormat kColor = VK_FORMAT_R8G8B8A8_SRGB, kDepth = VK_FORMAT_D32_SFLOAT;

    // Geometry with the tint applied (a copy: the caller's mesh stays as it is).
    MeshData md = mesh;
    for (Vertex& vert : md.vertices) vert.color *= v.tint;
    Buffer vb = dev.createBuffer(md.vertices.size() * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 MemoryUsage::GpuOnly, "icon vertices");
    Buffer ib = dev.createBuffer(md.indices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 MemoryUsage::GpuOnly, "icon indices");
    dev.uploadBuffer(vb, md.vertices.data(), md.vertices.size() * sizeof(Vertex));
    dev.uploadBuffer(ib, md.indices.data(), md.indices.size() * sizeof(uint32_t));

    ImageDesc cd;
    cd.format = kColor;
    cd.extent = {big, big, 1};
    cd.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    cd.debugName = "icon (supersampled)";
    Image color = dev.createImage(cd);
    ImageDesc dd = cd;
    dd.format = kDepth;
    dd.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    dd.debugName = "icon depth";
    Image depth = dev.createImage(dd);
    ImageDesc fd = cd;
    fd.extent = {size, size, 1};
    fd.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    fd.debugName = "icon";
    Image out = dev.createImage(fd);

    // The view: orthographic, from the given direction, fitting the mesh.
    const AABB b = md.bounds();
    const glm::vec3 center = b.center();
    const float yaw = glm::radians(v.yawDegrees), pitch = glm::radians(v.pitchDegrees);
    const glm::vec3 forward = glm::normalize(glm::vec3(-std::sin(yaw) * std::cos(pitch), -std::sin(pitch), -std::cos(yaw) * std::cos(pitch)));
    const float radius = glm::length(b.extents()) + 0.01f;
    const glm::mat4 view = glm::lookAt(center - forward * radius * 3.0f, center, glm::vec3(0, 1, 0));
    glm::vec2 lo(1e30f), hi(-1e30f);
    float zmin = 1e30f, zmax = -1e30f;
    for (const Vertex& vert : md.vertices) {
        const glm::vec3 p = glm::vec3(view * glm::vec4(vert.position, 1.0f));
        lo = glm::min(lo, glm::vec2(p));
        hi = glm::max(hi, glm::vec2(p));
        zmin = std::min(zmin, -p.z);
        zmax = std::max(zmax, -p.z);
    }
    const glm::vec2 mid = (lo + hi) * 0.5f;
    const float half = std::max(hi.x - lo.x, hi.y - lo.y) * 0.5f / std::max(1.0f - 2.0f * v.padding, 0.1f);
    glm::mat4 proj = glm::orthoRH_ZO(mid.x - half, mid.x + half, mid.y - half, mid.y + half, zmin - 0.1f, zmax + 0.1f);
    proj[1][1] *= -1.0f; // Vulkan's y points down
    IconPush push{proj * view, glm::vec4(glm::normalize(v.sunDirection), 0.0f), glm::vec4(v.sunColor, 1.0f), glm::vec4(v.skyColor, 0.0f),
                  glm::vec4(v.groundColor, 0.0f)};

    VkShaderModule vs = r.loadShader("icon.vert"), fs = r.loadShader("icon.frag");
    const auto binding = Vertex::binding();
    const auto attrs = Vertex::attributes();
    VkPipeline pipeline = PipelineBuilder()
                              .shaders(vs, fs)
                              .vertexInput({&binding, 1}, attrs)
                              .cull(CullMode::None)
                              .colorAttachment(kColor)
                              .depthAttachment(kDepth)
                              .depthTest(true, true, VK_COMPARE_OP_LESS)
                              .layout(r.pipelineLayout())
                              .build(dev.handle());
    vkDestroyShaderModule(dev.handle(), vs, nullptr);
    vkDestroyShaderModule(dev.handle(), fs, nullptr);

    dev.immediateSubmit([&](VkCommandBuffer cmd) {
        const VkImageSubresourceRange colorRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        const VkImageSubresourceRange depthRange{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        vk::imageBarrier(cmd, color.handle, colorRange, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        vk::imageBarrier(cmd, depth.handle, depthRange, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        ca.imageView = color.view;
        ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ca.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
        VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        da.imageView = depth.view;
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        da.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        da.clearValue.depthStencil = {1.0f, 0};
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {big, big}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
        ri.pDepthAttachment = &da;
        vkCmdBeginRendering(cmd, &ri);
        const VkViewport vp{0.0f, 0.0f, float(big), float(big), 0.0f, 1.0f};
        const VkRect2D sc{{0, 0}, {big, big}};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdPushConstants(cmd, r.pipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vb.handle, &offset);
        vkCmdBindIndexBuffer(cmd, ib.handle, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, uint32_t(md.indices.size()), 1, 0, 0, 0);
        vkCmdEndRendering(cmd);

        // Filter down into the icon.
        vk::imageBarrier(cmd, color.handle, colorRange, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                         VK_ACCESS_2_TRANSFER_READ_BIT);
        vk::imageBarrier(cmd, out.handle, colorRange, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                         VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {int32_t(big), int32_t(big), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[1] = {int32_t(size), int32_t(size), 1};
        vkCmdBlitImage(cmd, color.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       VK_FILTER_LINEAR);
        vk::imageBarrier(cmd, out.handle, colorRange, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });

    vkDestroyPipeline(dev.handle(), pipeline, nullptr);
    dev.destroyImage(color);
    dev.destroyImage(depth);
    dev.destroyBuffer(vb);
    dev.destroyBuffer(ib);
    return Texture::adopt(r, out, SamplerType::LinearClamp, "icon");
}

std::vector<IconPart> iconParts(GameObject& root)
{
    std::vector<IconPart> parts;
    const glm::mat4 toRoot = glm::inverse(root.transform().worldMatrix());
    std::function<void(GameObject&)> visit = [&](GameObject& o) {
        if (!o.activeInHierarchy()) return;
        for (const auto& c : o.components())
            if (auto* mr = dynamic_cast<MeshRenderer*>(c.get()); mr && mr->enabled && mr->visible && mr->mesh)
                parts.push_back({mr->mesh, mr->materials, toRoot * o.transform().worldMatrix()});
        for (Transform* ch : o.transform().children()) visit(ch->gameObject());
    };
    visit(root);
    return parts;
}

std::vector<uint8_t> renderIconPixels(Renderer& r, const std::vector<IconPart>& parts, uint32_t size, const IconView& v)
{
    std::vector<uint8_t> pixels;
    if (parts.empty() || size == 0) return pixels;
    Device& dev = r.device();
    const uint32_t big = size * 2; // supersampled, then filtered down
    constexpr VkFormat kColor = VK_FORMAT_R8G8B8A8_SRGB, kDepth = VK_FORMAT_D32_SFLOAT;

    // The view: orthographic, from the given direction, fitting every part's bounds.
    const float yaw = glm::radians(v.yawDegrees), pitch = glm::radians(v.pitchDegrees);
    const glm::vec3 forward = glm::normalize(glm::vec3(-std::sin(yaw) * std::cos(pitch), -std::sin(pitch), -std::cos(yaw) * std::cos(pitch)));
    AABB all;
    std::vector<glm::vec3> corners;
    for (const IconPart& p : parts) {
        const AABB b = p.mesh->bounds();
        if (!b.valid()) continue;
        for (int k = 0; k < 8; ++k) {
            const glm::vec3 c(k & 1 ? b.max.x : b.min.x, k & 2 ? b.max.y : b.min.y, k & 4 ? b.max.z : b.min.z);
            corners.push_back(glm::vec3(p.transform * glm::vec4(c, 1.0f)));
            all.expand(corners.back());
        }
    }
    if (!all.valid()) return pixels;
    const glm::vec3 center = all.center();
    const float radius = glm::length(all.extents()) + 0.01f;
    const glm::mat4 view = glm::lookAt(center - forward * radius * 3.0f, center, glm::vec3(0, 1, 0));
    glm::vec2 lo(1e30f), hi(-1e30f);
    float zmin = 1e30f, zmax = -1e30f;
    for (const glm::vec3& c : corners) {
        const glm::vec3 q = glm::vec3(view * glm::vec4(c, 1.0f));
        lo = glm::min(lo, glm::vec2(q));
        hi = glm::max(hi, glm::vec2(q));
        zmin = std::min(zmin, -q.z);
        zmax = std::max(zmax, -q.z);
    }
    const glm::vec2 mid = (lo + hi) * 0.5f;
    const float half = std::max(hi.x - lo.x, hi.y - lo.y) * 0.5f / std::max(1.0f - 2.0f * v.padding, 0.1f);
    glm::mat4 proj = glm::orthoRH_ZO(mid.x - half, mid.x + half, mid.y - half, mid.y + half, zmin - 0.1f, zmax + 0.1f);
    proj[1][1] *= -1.0f; // Vulkan's y points down
    const glm::mat4 viewProj = proj * view;

    ImageDesc cd;
    cd.format = kColor;
    cd.extent = {big, big, 1};
    cd.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    cd.debugName = "material icon (supersampled)";
    Image color = dev.createImage(cd);
    ImageDesc dd = cd;
    dd.format = kDepth;
    dd.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    dd.debugName = "material icon depth";
    Image depth = dev.createImage(dd);
    ImageDesc fd = cd;
    fd.extent = {size, size, 1};
    fd.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    fd.debugName = "material icon";
    Image out = dev.createImage(fd);
    Buffer staging = dev.createBuffer(VkDeviceSize(size) * size * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::GpuToCpu, "icon readback");

    VkShaderModule vs = r.loadShader("icon_material.vert"), fs = r.loadShader("icon_material.frag");
    const auto binding = Vertex::binding();
    const auto attrs = Vertex::attributes();
    VkPipeline pipeline = PipelineBuilder()
                              .shaders(vs, fs)
                              .vertexInput({&binding, 1}, attrs)
                              .cull(CullMode::None)
                              .colorAttachment(kColor)
                              .depthAttachment(kDepth)
                              .depthTest(true, true, VK_COMPARE_OP_LESS)
                              .layout(r.pipelineLayout())
                              .build(dev.handle());
    vkDestroyShaderModule(dev.handle(), vs, nullptr);
    vkDestroyShaderModule(dev.handle(), fs, nullptr);
    r.uploadMaterialsNow(); // the shader reads material parameters from the GPU buffer

    dev.immediateSubmit([&](VkCommandBuffer cmd) {
        const VkImageSubresourceRange colorRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        const VkImageSubresourceRange depthRange{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        vk::imageBarrier(cmd, color.handle, colorRange, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        vk::imageBarrier(cmd, depth.handle, depthRange, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        ca.imageView = color.view;
        ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ca.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
        VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        da.imageView = depth.view;
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        da.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        da.clearValue.depthStencil = {1.0f, 0};
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {big, big}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
        ri.pDepthAttachment = &da;
        vkCmdBeginRendering(cmd, &ri);
        const VkViewport vp{0.0f, 0.0f, float(big), float(big), 0.0f, 1.0f};
        const VkRect2D sc{{0, 0}, {big, big}};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        r.bindGlobalSets(cmd); // materials, bindless textures
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        for (const IconPart& p : parts) {
            p.mesh->bind(cmd);
            const glm::quat q = glm::quat_cast(glm::mat3(glm::normalize(glm::vec3(p.transform[0])), glm::normalize(glm::vec3(p.transform[1])),
                                                         glm::normalize(glm::vec3(p.transform[2]))));
            for (const Submesh& sm : p.mesh->submeshes()) {
                const Material* mat = p.materials.empty() ? r.fallbackMaterial()
                                                          : p.materials[std::min<size_t>(sm.materialSlot, p.materials.size() - 1)].get();
                if (!mat) mat = r.fallbackMaterial();
                MaterialIconPush push{viewProj * p.transform, glm::vec4(q.x, q.y, q.z, q.w),
                                      glm::vec4(glm::normalize(v.sunDirection), std::bit_cast<float>(mat->gpuIndex())),
                                      glm::vec4(v.sunColor, 1.0f), glm::vec4(v.skyColor, glm::length(v.groundColor) / std::max(glm::length(v.skyColor), 1e-4f))};
                vkCmdPushConstants(cmd, r.pipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
                vkCmdDrawIndexed(cmd, sm.indexCount, 1, sm.firstIndex, 0, 0);
            }
        }
        vkCmdEndRendering(cmd);

        // Filter down, then read back.
        vk::imageBarrier(cmd, color.handle, colorRange, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                         VK_ACCESS_2_TRANSFER_READ_BIT);
        vk::imageBarrier(cmd, out.handle, colorRange, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                         VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {int32_t(big), int32_t(big), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[1] = {int32_t(size), int32_t(size), 1};
        vkCmdBlitImage(cmd, color.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       VK_FILTER_LINEAR);
        vk::imageBarrier(cmd, out.handle, colorRange, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {size, size, 1};
        vkCmdCopyImageToBuffer(cmd, out.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.handle, 1, &region);
    });
    vmaInvalidateAllocation(dev.allocator(), staging.allocation, 0, VK_WHOLE_SIZE);
    const auto* bytes = static_cast<const uint8_t*>(staging.mapped);
    pixels.assign(bytes, bytes + size_t(size) * size * 4);

    vkDestroyPipeline(dev.handle(), pipeline, nullptr);
    dev.destroyImage(color);
    dev.destroyImage(depth);
    dev.destroyImage(out);
    dev.destroyBuffer(staging);
    return pixels;
}

} // namespace ve::ui
