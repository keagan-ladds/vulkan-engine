#include "ve/render/Impostor.hpp"

#include "ve/gfx/Device.hpp"
#include "ve/gfx/Pipeline.hpp"
#include "ve/render/Mesh.hpp"
#include "ve/render/Renderer.hpp"
#include "ve/render/Texture.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <bit>

namespace ve {

namespace {

// Same as veHemiOctDecode / veImpostorBasis in ve_impostor.glsl.
glm::vec3 hemiOctDecode(glm::vec2 uv)
{
    const glm::vec2 f = uv * 2.0f - 1.0f;
    glm::vec3 d((f.x + f.y) * 0.5f, 0.0f, (f.x - f.y) * 0.5f);
    d.y = 1.0f - std::abs(d.x) - std::abs(d.z);
    return glm::normalize(d);
}

void impostorBasis(const glm::vec3& d, glm::vec3& right, glm::vec3& up)
{
    right = std::abs(d.y) < 0.999f ? glm::normalize(glm::cross(glm::vec3(0, 1, 0), d)) : glm::vec3(1, 0, 0);
    up = glm::cross(d, right);
}

struct BakePush {
    glm::mat4 viewProj;
    glm::vec4 centreRadius;
    uint32_t materialIndex;
};

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, uint32_t baseMip, uint32_t mipCount,
                  VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                  VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, baseMip, mipCount, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

// Mip chain for an image whose mip 0 is in COLOR_ATTACHMENT_OPTIMAL; leaves
// every level READ_ONLY_OPTIMAL.
void buildMips(VkCommandBuffer cmd, const Image& img)
{
    int32_t w = static_cast<int32_t>(img.extent.width), h = static_cast<int32_t>(img.extent.height);
    imageBarrier(cmd, img.handle, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    if (img.mipLevels > 1) {
        imageBarrier(cmd, img.handle, VK_IMAGE_ASPECT_COLOR_BIT, 1, img.mipLevels - 1, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    }
    for (uint32_t mip = 1; mip < img.mipLevels; ++mip) {
        const int32_t nw = std::max(1, w / 2), nh = std::max(1, h / 2);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 0, 1};
        blit.srcOffsets[1] = {w, h, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
        blit.dstOffsets[1] = {nw, nh, 1};
        vkCmdBlitImage(cmd, img.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img.handle,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        imageBarrier(cmd, img.handle, VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        w = nw;
        h = nh;
    }
    imageBarrier(cmd, img.handle, VK_IMAGE_ASPECT_COLOR_BIT, 0, img.mipLevels, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

} // namespace

std::shared_ptr<Impostor> Impostor::bake(Renderer& renderer, const Mesh& mesh,
                                         const std::vector<std::shared_ptr<Material>>& materials,
                                         const ImpostorSettings& settings)
{
    Device& device = renderer.device();
    const uint32_t N = std::max(2u, settings.frames);
    const uint32_t res = settings.frameResolution;
    const uint32_t size = N * res;
    // Stop the mip chain before frames shrink below ~8 px (they'd bleed together).
    const uint32_t mips = std::max(1, static_cast<int>(std::bit_width(res)) - 3);

    const AABB& bounds = mesh.bounds();
    const glm::vec3 centre = bounds.center();
    const float radius = glm::length(bounds.extents()) * 1.02f;

    // --- Render targets --------------------------------------------------------------
    ImageDesc color;
    color.extent = {size, size, 1};
    color.mipLevels = mips;
    color.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    color.format = VK_FORMAT_R8G8B8A8_SRGB;
    color.debugName = "impostor albedo";
    Image albedo = device.createImage(color);
    color.format = VK_FORMAT_R8G8B8A8_UNORM;
    color.debugName = "impostor normal";
    Image normal = device.createImage(color);
    ImageDesc depthDesc;
    depthDesc.format = VK_FORMAT_D32_SFLOAT;
    depthDesc.extent = {size, size, 1};
    depthDesc.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    Image depth = device.createImage(depthDesc);
    // Attachments must be single-mip views.
    VkImageView albedoView = device.createImageView(albedo.handle, albedo.format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1);
    VkImageView normalView = device.createImageView(normal.handle, normal.format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1);

    // --- Bake pipeline --------------------------------------------------------------------
    const auto binding = Vertex::binding();
    const auto attributes = Vertex::attributes();
    VkShaderModule vs = renderer.loadShader("impostor_bake.vert");
    VkShaderModule fs = renderer.loadShader("impostor_bake.frag");
    VkPipeline pipeline = PipelineBuilder()
                              .shaders(vs, fs)
                              .vertexInput({&binding, 1}, attributes)
                              .colorAttachment(VK_FORMAT_R8G8B8A8_SRGB)
                              .colorAttachment(VK_FORMAT_R8G8B8A8_UNORM)
                              .depthAttachment(VK_FORMAT_D32_SFLOAT)
                              .depthTest(true, true, VK_COMPARE_OP_LESS_OR_EQUAL)
                              // The bake projection has no Y flip (unlike the main camera), which
                              // mirrors triangle winding: CCW-front meshes appear clockwise here.
                              // Without this, gl_FrontFacing is inverted and normals bake upside down.
                              .cull(CullMode::None, VK_FRONT_FACE_CLOCKWISE)
                              .layout(renderer.pipelineLayout())
                              .build(device.handle());
    vkDestroyShaderModule(device.handle(), vs, nullptr);
    vkDestroyShaderModule(device.handle(), fs, nullptr);

    renderer.uploadMaterialsNow(); // the bake shader reads material parameters from the GPU buffer

    device.immediateSubmit([&](VkCommandBuffer cmd) {
        for (VkImage img : {albedo.handle, normal.handle}) {
            imageBarrier(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        }
        imageBarrier(cmd, depth.handle, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

        VkRenderingAttachmentInfo colors[2]{};
        const VkImageView views[2] = {albedoView, normalView};
        // Transparent texels keep a neutral colour so mip blending doesn't darken edges.
        const VkClearColorValue clears[2] = {{{0.35f, 0.33f, 0.2f, 0.0f}}, {{0.5f, 1.0f, 0.5f, 0.0f}}};
        for (int i = 0; i < 2; ++i) {
            colors[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            colors[i].imageView = views[i];
            colors[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            colors[i].clearValue.color = clears[i];
        }
        VkRenderingAttachmentInfo depthInfo{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depthInfo.imageView = depth.view;
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthInfo.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthInfo.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthInfo.clearValue.depthStencil = {1.0f, 0};
        VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
        info.renderArea = {{0, 0}, {size, size}};
        info.layerCount = 1;
        info.colorAttachmentCount = 2;
        info.pColorAttachments = colors;
        info.pDepthAttachment = &depthInfo;
        vkCmdBeginRendering(cmd, &info);

        renderer.bindGlobalSets(cmd);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        VkBuffer vb = mesh.vertexBuffer();
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
        vkCmdBindIndexBuffer(cmd, mesh.indexBuffer(), 0, VK_INDEX_TYPE_UINT32);

        for (uint32_t j = 0; j < N; ++j) {
            for (uint32_t i = 0; i < N; ++i) {
                // One orthographic view per frame, looking from direction d at the centre.
                const glm::vec3 d = hemiOctDecode(glm::vec2(float(i), float(j)) / float(N - 1));
                glm::vec3 right, up;
                impostorBasis(d, right, up);
                const glm::mat4 view = glm::lookAt(centre + d * radius * 2.0f, centre, up);
                const glm::mat4 proj = glm::ortho(-radius, radius, -radius, radius, 0.0f, radius * 4.0f);
                const VkViewport vp{float(i * res), float(j * res), float(res), float(res), 0.0f, 1.0f};
                const VkRect2D scissor{{int32_t(i * res), int32_t(j * res)}, {res, res}};
                vkCmdSetViewport(cmd, 0, 1, &vp);
                vkCmdSetScissor(cmd, 0, 1, &scissor);
                for (const Submesh& sm : mesh.submeshes()) {
                    const Material* mat = materials.empty()
                                              ? renderer.fallbackMaterial()
                                              : materials[std::min<size_t>(sm.materialSlot, materials.size() - 1)].get();
                    const BakePush push{proj * view, glm::vec4(centre, radius), mat->gpuIndex()};
                    vkCmdPushConstants(cmd, renderer.pipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
                    vkCmdDrawIndexed(cmd, sm.indexCount, 1, sm.firstIndex, 0, 0);
                }
            }
        }
        vkCmdEndRendering(cmd);
        buildMips(cmd, albedo);
        buildMips(cmd, normal);
    });

    vkDestroyPipeline(device.handle(), pipeline, nullptr);
    vkDestroyImageView(device.handle(), albedoView, nullptr);
    vkDestroyImageView(device.handle(), normalView, nullptr);
    device.destroyImage(depth);

    auto imp = std::shared_ptr<Impostor>(new Impostor());
    imp->m_albedo = Texture::adopt(renderer, albedo, SamplerType::LinearClamp, "impostor albedo");
    imp->m_normal = Texture::adopt(renderer, normal, SamplerType::LinearClamp, "impostor normal");
    imp->m_material = renderer.createMaterial(renderer.builtinShader(BuiltinShader::Impostor));
    imp->m_material->setVector(0, glm::vec4(centre, radius));
    imp->m_material->setVector(1, glm::vec4(float(N), settings.alphaCutoff, 0.0f, 0.0f));
    imp->m_material->setTexture(0, imp->m_albedo);
    imp->m_material->setTexture(1, imp->m_normal);

    // The quad's vertices only carry UVs (the shader builds the billboard), but
    // its positions span the bounding sphere so its bounds are meaningful.
    MeshData quad;
    const glm::vec2 uvs[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
    for (const glm::vec2& uv : uvs) {
        Vertex v;
        v.position = centre + glm::vec3((uv.x * 2.0f - 1.0f) * radius, (1.0f - uv.y * 2.0f) * radius, 0.0f);
        v.uv = uv;
        quad.vertices.push_back(v);
    }
    quad.indices = {0, 1, 2, 0, 2, 3};
    imp->m_quad = Mesh::create(renderer, quad, {}, "impostor quad");
    return imp;
}

} // namespace ve
