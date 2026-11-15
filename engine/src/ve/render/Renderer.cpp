#include "ve/render/Renderer.hpp"

#include "ve/core/Log.hpp"
#include "ve/core/Time.hpp"
#include "ve/core/Window.hpp"
#include "ve/render/Mesh.hpp"
#include "ve/render/Texture.hpp"
#include "ve/render/passes/BuiltinPasses.hpp"
#include "ve/scene/Components.hpp"
#include "ve/scene/Scene.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace ve {

Renderer::~Renderer()
{
    shutdown();
}

// -----------------------------------------------------------------------------
// Init / shutdown
// -----------------------------------------------------------------------------

void Renderer::init(Window& window, const RendererSettings& settings, bool validation,
                    std::vector<std::filesystem::path> shaderDirectories)
{
    m_window = &window;
    m_settings = settings;
    m_shaderDirs = std::move(shaderDirectories);

    m_device.init(window, validation);
    m_samplers.init(m_device);
    m_bindless.init(m_device);
    m_swapchain.create(m_device, window.drawableSize(), m_settings.vsync);

    m_materials.resize(kMaxMaterials);
    m_lights = std::make_unique<LightBuffer>();
    m_frame.lights = m_lights.get();

    createGlobalLayout();
    createFrameSlots();

    m_graph = std::make_unique<RenderGraph>(*this);
    m_graph->resize(m_swapchain.extent());
    m_sceneColor = m_graph->createTexture("SceneColor", {.format = kSceneColorFormat});
    m_sceneDepth = m_graph->createTexture("SceneDepth", {.format = kDepthFormat, .sampler = SamplerType::NearestClamp});
    m_sceneNormal = m_graph->createTexture("SceneNormal", {.format = kSceneNormalFormat, .sampler = SamplerType::NearestClamp});
    m_backbuffer = m_graph->importTexture("Backbuffer");
    if (m_settings.headless) {
        m_headlessTarget = m_device.createImage({.format = m_swapchain.format(),
                                                 .extent = {m_swapchain.extent().width, m_swapchain.extent().height, 1},
                                                 .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                                 .debugName = "headless backbuffer"});
        log::info("Renderer: headless (offscreen, nothing presented)");
        if (const char* c = std::getenv("VE_CAPTURE"); c && *c) {
            m_envCapturePath = c;
            const char* f = std::getenv("VE_CAPTURE_FRAME");
            m_envCaptureFrame = f && *f ? std::strtoull(f, nullptr, 10) : 150;
        }
    }
    createShadowMaps();
    writeShadowDescriptors();

    createDefaults();
    m_instances = std::make_unique<InstanceSystem>();
    m_instances->init(*this, m_settings.maxInstances, m_settings.maxVisibleInstancesPerView,
                      1 + m_settings.maxShadowedDirectionalLights * m_settings.cascadeCount);
    writeInstanceDescriptors();
    createDefaultPasses();
    m_initialized = true;
}

void Renderer::shutdown()
{
    if (!m_initialized) return;
    m_device.waitIdle();

    // Release everything that defers its destruction through the device first.
    // Freed bindless slots normally get re-pointed at the white texture; it is
    // about to be destroyed itself, so stop doing that.
    m_bindless.setFallback(VK_NULL_HANDLE, VK_NULL_HANDLE);
    m_graph.reset();
    if (m_instances) m_instances->shutdown();
    m_fallbackMaterial.reset();
    m_builtinShaders = {};
    m_white.reset();
    m_black.reset();
    m_flatNormal.reset();
    // Run all deferred deletions now, while the bindless set etc. still exist.
    m_device.flushDeletions();

    VkDevice dev = m_device.handle();
    for (FrameSlot& s : m_slots) {
        m_device.destroyBuffer(s.globals);
        m_device.destroyBuffer(s.lights);
        m_device.destroyBuffer(s.materials);
        if (s.timestamps) vkDestroyQueryPool(dev, s.timestamps, nullptr);
        vkDestroyFence(dev, s.inFlight, nullptr);
        vkDestroySemaphore(dev, s.imageAvailable, nullptr);
        vkDestroyCommandPool(dev, s.pool, nullptr);
    }
    vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr);
    vkDestroyDescriptorPool(dev, m_globalPool, nullptr);
    vkDestroyDescriptorSetLayout(dev, m_globalSetLayout, nullptr);
    if (m_headlessTarget.handle) m_device.destroyImage(m_headlessTarget);
    if (m_captureBuffer.handle) m_device.destroyBuffer(m_captureBuffer);
    m_swapchain.destroy(m_device);
    m_bindless.shutdown();
    m_samplers.shutdown(m_device);
    m_device.shutdown(); // flushes all deferred deletions
    m_initialized = false;
}

void Renderer::createGlobalLayout()
{
    VkDevice dev = m_device.handle();

    // set 0: per-frame data shared by every shader (see ve.glsl).
    VkDescriptorSetLayoutBinding b[10]{};
    b[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr};
    b[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr};
    b[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr};
    b[3] = {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_ALL, nullptr};
    b[4] = {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_ALL, nullptr};
    // GPU instancing: instances, visible lists, indirect commands, groups, cull views.
    for (uint32_t i = 5; i < 10; ++i) b[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr};
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 10;
    layoutInfo.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(dev, &layoutInfo, nullptr, &m_globalSetLayout));

    const uint32_t n = Device::kFramesInFlight;
    VkDescriptorPoolSize sizes[3] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, n},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 7 * n},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * n},
    };
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = n;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(dev, &poolInfo, nullptr, &m_globalPool));

    // The single pipeline layout used by every pipeline in the engine:
    // set 0 = globals, set 1 = bindless, 128 bytes of push constants.
    VkDescriptorSetLayout sets[2] = {m_globalSetLayout, m_bindless.layout()};
    VkPushConstantRange push{VK_SHADER_STAGE_ALL, 0, 128};
    VkPipelineLayoutCreateInfo plInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plInfo.setLayoutCount = 2;
    plInfo.pSetLayouts = sets;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges = &push;
    VK_CHECK(vkCreatePipelineLayout(dev, &plInfo, nullptr, &m_pipelineLayout));
}

void Renderer::createFrameSlots()
{
    VkDevice dev = m_device.handle();
    for (FrameSlot& s : m_slots) {
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.queueFamilyIndex = m_device.graphicsFamily();
        VK_CHECK(vkCreateCommandPool(dev, &poolInfo, nullptr, &s.pool));
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = s.pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(dev, &alloc, &s.cmd));

        VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(dev, &semInfo, nullptr, &s.imageAvailable));
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(dev, &fenceInfo, nullptr, &s.inFlight));

        if (m_device.properties().limits.timestampComputeAndGraphics) {
            VkQueryPoolCreateInfo q{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            q.queryType = VK_QUERY_TYPE_TIMESTAMP;
            q.queryCount = kMaxTimestamps;
            VK_CHECK(vkCreateQueryPool(dev, &q, nullptr, &s.timestamps));
            m_timestampPeriodNs = m_device.properties().limits.timestampPeriod;
        }

        s.globals = m_device.createBuffer(sizeof(FrameGlobals), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                          MemoryUsage::CpuToGpu, "FrameGlobals");
        s.lights = m_device.createBuffer(sizeof(LightBuffer), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                         MemoryUsage::CpuToGpu, "LightBuffer");
        s.materials = m_device.createBuffer(sizeof(GpuMaterial) * kMaxMaterials, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                            MemoryUsage::CpuToGpu, "MaterialBuffer");

        VkDescriptorSetAllocateInfo setAlloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        setAlloc.descriptorPool = m_globalPool;
        setAlloc.descriptorSetCount = 1;
        setAlloc.pSetLayouts = &m_globalSetLayout;
        VK_CHECK(vkAllocateDescriptorSets(dev, &setAlloc, &s.set));

        VkDescriptorBufferInfo bufs[3] = {
            {s.globals.handle, 0, VK_WHOLE_SIZE},
            {s.lights.handle, 0, VK_WHOLE_SIZE},
            {s.materials.handle, 0, VK_WHOLE_SIZE},
        };
        VkWriteDescriptorSet writes[3]{};
        for (uint32_t i = 0; i < 3; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = s.set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &bufs[i];
        }
        vkUpdateDescriptorSets(dev, 3, writes, 0, nullptr);
    }
}

void Renderer::createShadowMaps()
{
    m_settings.maxShadowedDirectionalLights = std::min(m_settings.maxShadowedDirectionalLights, kMaxDirectionalLights);
    m_settings.cascadeCount = std::clamp(m_settings.cascadeCount, 1u, kMaxCascades);
    const uint32_t dirViews = kMaxDirectionalLights * kMaxCascades;
    m_settings.maxShadowedPointLights = std::min(m_settings.maxShadowedPointLights, (kMaxShadowViews - dirViews) / 6);

    GraphTextureDesc dir;
    dir.format = kShadowFormat;
    dir.width = dir.height = m_settings.directionalShadowResolution;
    dir.layers = std::max(1u, m_settings.maxShadowedDirectionalLights * m_settings.cascadeCount);
    dir.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    dir.perLayerViews = true;

    GraphTextureDesc point;
    point.format = kShadowFormat;
    point.width = point.height = m_settings.pointShadowResolution;
    point.layers = 6 * std::max(1u, m_settings.maxShadowedPointLights);
    point.viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    point.perLayerViews = true;

    if (m_dirShadowMap == kNoTexture) {
        m_dirShadowMap = m_graph->createTexture("DirShadowMap", dir);
        m_pointShadowMap = m_graph->createTexture("PointShadowMap", point);
    } else {
        m_graph->recreateTexture(m_dirShadowMap, dir);
        m_graph->recreateTexture(m_pointShadowMap, point);
    }
    // Clear to "far" and make them sampleable even before the first shadow pass.
    m_graph->initializeDepth(m_dirShadowMap, 1.0f);
    m_graph->initializeDepth(m_pointShadowMap, 1.0f);
}

void Renderer::writeShadowDescriptors()
{
    VkSampler cmp = m_samplers.get(SamplerType::ShadowCompare);
    for (FrameSlot& s : m_slots) {
        VkDescriptorImageInfo imgs[2] = {
            {cmp, m_graph->get(m_dirShadowMap).image.view, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL},
            {cmp, m_graph->get(m_pointShadowMap).image.view, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL},
        };
        VkWriteDescriptorSet writes[2]{};
        for (uint32_t i = 0; i < 2; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = s.set;
            writes[i].dstBinding = 3 + i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo = &imgs[i];
        }
        vkUpdateDescriptorSets(m_device.handle(), 2, writes, 0, nullptr);
    }
}

void Renderer::writeInstanceDescriptors()
{
    for (uint32_t slot = 0; slot < Device::kFramesInFlight; ++slot) {
        const VkDescriptorBufferInfo bufs[5] = {
            {m_instances->instanceBuffer(), 0, VK_WHOLE_SIZE},
            {m_instances->visibleBuffer(), 0, VK_WHOLE_SIZE},
            {m_instances->commandBuffer(), 0, VK_WHOLE_SIZE},
            {m_instances->groupBuffer(slot), 0, VK_WHOLE_SIZE},
            {m_instances->viewBuffer(slot), 0, VK_WHOLE_SIZE},
        };
        VkWriteDescriptorSet writes[5]{};
        for (uint32_t i = 0; i < 5; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = m_slots[slot].set;
            writes[i].dstBinding = 5 + i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &bufs[i];
        }
        vkUpdateDescriptorSets(m_device.handle(), 5, writes, 0, nullptr);
    }
}

std::shared_ptr<InstanceGroup> Renderer::createInstanceGroup(InstanceGroupDesc desc, uint32_t capacity)
{
    return m_instances->createGroup(std::move(desc), capacity);
}

void Renderer::createDefaults()
{
    m_white = Texture::solid(*this, 255, 255, 255, 255, true, "white");
    m_black = Texture::solid(*this, 0, 0, 0, 255, true, "black");
    m_flatNormal = Texture::solid(*this, 128, 128, 255, 255, false, "flatNormal");
    m_bindless.setFallback(m_white->image().view, m_samplers.get(SamplerType::LinearRepeat));

    ShaderDesc standard;
    standard.name = "Standard";
    m_builtinShaders[size_t(BuiltinShader::Standard)] = createShader(standard);

    ShaderDesc cutout = standard;
    cutout.name = "StandardCutout";
    cutout.cull = CullMode::None;
    cutout.queue = RenderQueue::AlphaTest;
    cutout.shadowFragment = "shadow_cutout.frag"; // shadows with holes (leaves, fences)
    cutout.shadowPointFragment = "shadow_point_cutout.frag";
    m_builtinShaders[size_t(BuiltinShader::StandardCutout)] = createShader(cutout);

    ShaderDesc transparent = standard;
    transparent.name = "StandardTransparent";
    transparent.blend = BlendMode::AlphaBlend;
    transparent.depthWrite = false;
    transparent.castShadows = false;
    transparent.queue = RenderQueue::Transparent;
    m_builtinShaders[size_t(BuiltinShader::StandardTransparent)] = createShader(transparent);

    ShaderDesc unlit = standard;
    unlit.name = "Unlit";
    unlit.fragment = "unlit.frag";
    unlit.castShadows = false;
    m_builtinShaders[size_t(BuiltinShader::Unlit)] = createShader(unlit);

    ShaderDesc impostor = standard;
    impostor.name = "Impostor";
    impostor.vertex = "impostor.vert";
    impostor.fragment = "impostor.frag";
    impostor.shadowVertex = "impostor_shadow.vert";
    impostor.shadowFragment = "impostor_shadow.frag";
    impostor.cull = CullMode::None;
    impostor.queue = RenderQueue::AlphaTest;
    m_builtinShaders[size_t(BuiltinShader::Impostor)] = createShader(impostor);

    m_fallbackMaterial = createStandardMaterial();
    m_fallbackMaterial->setBaseColor({0.8f, 0.8f, 0.8f, 1.0f});
}

void Renderer::createDefaultPasses()
{
    m_graph->addPass<InstanceCullPass>();
    m_graph->addPass<ShadowPass>();
    m_graph->addPass<OpaquePass>();
    m_graph->addPass<TransparentPass>();
    m_graph->addPass<CompositePass>();
}

void Renderer::applySettings()
{
    m_device.waitIdle();
    createShadowMaps();
    writeShadowDescriptors();
    m_swapchainDirty = true; // picks up vsync changes
}

bool Renderer::recreateSwapchain()
{
    const VkExtent2D size = m_window->drawableSize();
    if (size.width == 0 || size.height == 0) return false;
    m_device.waitIdle();
    m_swapchain.destroy(m_device);
    m_swapchain.create(m_device, size, m_settings.vsync);
    m_graph->resize(m_swapchain.extent());
    m_swapchainDirty = false;
    return true;
}

// -----------------------------------------------------------------------------
// Resources
// -----------------------------------------------------------------------------

std::shared_ptr<Shader> Renderer::createShader(const ShaderDesc& desc)
{
    return std::make_shared<Shader>(*this, desc);
}

std::shared_ptr<Material> Renderer::createMaterial(std::shared_ptr<Shader> shader)
{
    return std::make_shared<Material>(*this, std::move(shader));
}

std::shared_ptr<StandardMaterial> Renderer::createStandardMaterial(StandardMaterial::Mode mode)
{
    return std::make_shared<StandardMaterial>(*this, mode);
}

void Renderer::setGlobalParam(uint32_t index, const glm::vec4& value)
{
    if (index < kGlobalParamCount) m_globalParams[index] = value;
}

glm::vec4 Renderer::globalParam(uint32_t index) const
{
    return index < kGlobalParamCount ? m_globalParams[index] : glm::vec4(0.0f);
}

std::filesystem::path Renderer::findShader(const std::string& name) const
{
    for (const auto& dir : m_shaderDirs) {
        auto p = dir / (name + ".spv");
        if (std::filesystem::exists(p)) return p;
    }
    throw std::runtime_error("Shader not found: " + name + ".spv");
}

VkShaderModule Renderer::loadShader(const std::string& name)
{
    return loadShaderModule(m_device.handle(), findShader(name));
}

VkPipeline Renderer::buildMeshPipeline(const ShaderDesc& desc, PassKind kind)
{
    VkDevice dev = m_device.handle();
    const auto binding = Vertex::binding();
    const auto attributes = Vertex::attributes();

    PipelineBuilder b;
    b.layout(m_pipelineLayout);

    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
    if (kind == PassKind::Forward) {
        vert = loadShader(desc.vertex);
        frag = loadShader(desc.fragment);
        const bool wire = desc.wireframe && m_device.features().fillModeNonSolid;
        // Reversed-Z: closer = larger depth, so the test is GREATER_OR_EQUAL.
        b.shaders(vert, frag)
            .vertexInput({&binding, 1}, attributes)
            .cull(desc.cull)
            .polygonMode(wire ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL)
            .colorAttachment(kSceneColorFormat, desc.blend)
            // Opaque shaders also write the thin G-buffer; transparent ones leave it alone.
            .colorAttachment(kSceneNormalFormat, BlendMode::Opaque, desc.queue < RenderQueue::Transparent)
            .depthAttachment(kDepthFormat)
            .depthTest(desc.depthTest, desc.depthWrite, VK_COMPARE_OP_GREATER_OR_EQUAL);
    } else {
        // Shadow maps use standard depth (LESS). No culling: thin geometry
        // (leaves, open meshes) must still cast shadows from both sides.
        vert = loadShader(desc.shadowVertex);
        if (kind == PassKind::ShadowPoint) frag = loadShader(desc.shadowPointFragment);
        else if (!desc.shadowFragment.empty()) frag = loadShader(desc.shadowFragment); // e.g. alpha cutout
        b.shaders(vert, frag)
            .vertexInput({&binding, 1}, attributes)
            .cull(CullMode::None)
            .depthAttachment(kShadowFormat)
            .depthTest(true, true, VK_COMPARE_OP_LESS_OR_EQUAL)
            .dynamicDepthBias(true)
            .depthClamp(kind == PassKind::ShadowDirectional && m_device.features().depthClamp);
    }
    VkPipeline pipeline = b.build(dev);
    vkDestroyShaderModule(dev, vert, nullptr);
    if (frag) vkDestroyShaderModule(dev, frag, nullptr);
    return pipeline;
}

VkPipeline Renderer::createFullscreenPipeline(const FullscreenPipelineDesc& desc)
{
    VkDevice dev = m_device.handle();
    VkShaderModule vert = loadShader("fullscreen.vert");
    VkShaderModule frag = loadShader(desc.fragment);
    PipelineBuilder b;
    b.shaders(vert, frag).layout(m_pipelineLayout).cull(CullMode::None);
    for (VkFormat f : desc.colorFormats) b.colorAttachment(f, desc.blend);
    if (desc.depthFormat != VK_FORMAT_UNDEFINED) {
        b.depthAttachment(desc.depthFormat).depthTest(true, false, desc.depthCompare);
    }
    VkPipeline p = b.build(dev);
    vkDestroyShaderModule(dev, vert, nullptr);
    vkDestroyShaderModule(dev, frag, nullptr);
    return p;
}

VkPipeline Renderer::createComputePipeline(const std::string& shaderName)
{
    VkShaderModule module = loadShader(shaderName);
    VkPipeline p = ve::createComputePipeline(m_device.handle(), module, m_pipelineLayout);
    vkDestroyShaderModule(m_device.handle(), module, nullptr);
    return p;
}

void Renderer::destroyPipeline(VkPipeline pipeline)
{
    if (!pipeline) return;
    Device* device = &m_device;
    m_device.deferDestroy([device, pipeline] { vkDestroyPipeline(device->handle(), pipeline, nullptr); });
}

uint32_t Renderer::allocateMaterialSlot()
{
    if (!m_freeMaterials.empty()) {
        const uint32_t i = m_freeMaterials.back();
        m_freeMaterials.pop_back();
        return i;
    }
    if (m_materialHighWater >= kMaxMaterials) throw std::runtime_error("Too many materials");
    return m_materialHighWater++;
}

void Renderer::freeMaterialSlot(uint32_t index)
{
    // In-flight frames may still read this slot: recycle it later.
    m_device.deferDestroy([this, index] { m_freeMaterials.push_back(index); });
}

void Renderer::uploadMaterialsNow()
{
    for (FrameSlot& s : m_slots) {
        std::memcpy(s.materials.mapped, m_materials.data(), sizeof(GpuMaterial) * m_materialHighWater);
    }
}

void Renderer::bindGlobalSets(VkCommandBuffer cmd)
{
    const FrameSlot& slot = m_slots[m_frameNumber % Device::kFramesInFlight];
    VkDescriptorSet sets[2] = {slot.set, m_bindless.set()};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 2, sets, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 0, 2, sets, 0, nullptr);
}

// -----------------------------------------------------------------------------
// Drawing
// -----------------------------------------------------------------------------

void Renderer::drawItems(RenderContext& ctx, const std::vector<uint32_t>& indices, PassKind kind,
                         uint32_t viewIndex, uint32_t lightIndex, uint32_t flags)
{
    VkCommandBuffer cmd = ctx.cmd;
    VkPipeline boundPipeline = VK_NULL_HANDLE;
    const Mesh* boundMesh = nullptr;
    const bool shadow = kind != PassKind::Forward;

    for (uint32_t idx : indices) {
        const DrawItem& item = m_frame.items[idx];
        Shader& shader = item.material->shader();
        if (shadow && (!shader.desc().castShadows || !item.castShadows)) continue;

        VkPipeline p = shader.pipeline(kind);
        if (p != boundPipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
            boundPipeline = p;
        }
        if (item.mesh != boundMesh) {
            VkBuffer vb = item.mesh->vertexBuffer();
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
            vkCmdBindIndexBuffer(cmd, item.mesh->indexBuffer(), 0, VK_INDEX_TYPE_UINT32);
            boundMesh = item.mesh;
        }
        DrawPush push{};
        push.model = item.world;
        push.materialIndex = item.material->gpuIndex();
        push.viewIndex = viewIndex;
        push.lightIndex = lightIndex;
        push.flags = flags;
        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_ALL, 0, sizeof(DrawPush), &push);
        vkCmdDrawIndexed(cmd, item.submesh->indexCount, 1, item.submesh->firstIndex, 0, 0);

        if (shadow) ++m_frame.stats.shadowDrawCalls;
        else ++m_frame.stats.drawCalls;
        m_frame.stats.triangles += item.submesh->indexCount / 3;
    }
}

// -----------------------------------------------------------------------------
// Per-frame scene gathering
// -----------------------------------------------------------------------------

void Renderer::setupCamera(Scene& scene)
{
    CameraData& cam = m_frame.camera;
    const VkExtent2D ext = m_swapchain.extent();
    cam.aspect = static_cast<float>(ext.width) / static_cast<float>(std::max(1u, ext.height));

    glm::vec3 pos(0.0f, 2.0f, 5.0f);
    glm::quat rot(1.0f, 0.0f, 0.0f, 0.0f);
    if (Camera* c = scene.mainCamera()) {
        pos = c->transform().position();
        rot = c->transform().rotation();
        cam.fovY = glm::radians(c->fovDegrees);
        cam.nearPlane = c->nearPlane;
        cam.farPlane = c->farPlane;
    }
    cam.position = pos;
    cam.forward = rot * glm::vec3(0, 0, -1);
    cam.up = rot * glm::vec3(0, 1, 0);
    cam.right = rot * glm::vec3(1, 0, 0);
    // View = inverse of the camera's (unscaled) world transform.
    cam.view = glm::mat4_cast(glm::conjugate(rot)) * glm::translate(glm::mat4(1.0f), -pos);
    // Reversed-Z: pass far as "near" and near as "far" so depth is 1 at the
    // near plane and 0 at the far plane. Then flip Y for Vulkan's clip space.
    cam.proj = glm::perspective(cam.fovY, cam.aspect, cam.farPlane, cam.nearPlane);
    cam.proj[1][1] *= -1.0f;
    cam.viewProj = cam.proj * cam.view;
    cam.frustum = Frustum::fromMatrix(cam.viewProj);
}

void Renderer::gatherScene(Scene& scene)
{
    m_frame.items.clear();
    m_frame.opaque.clear();
    m_frame.transparent.clear();
    m_frame.shadowViews.clear();
    m_frame.stats = {};

    setupCamera(scene);
    const CameraData& cam = m_frame.camera;

    scene.forEach<MeshRenderer>([&](MeshRenderer& mr) {
        if (!mr.isActive() || !mr.visible || !mr.mesh) return;
        const glm::mat4& world = mr.transform().worldMatrix();
        for (const Submesh& sm : mr.mesh->submeshes()) {
            Material* mat = mr.material(sm.materialSlot);
            if (!mat) mat = m_fallbackMaterial.get();
            DrawItem item;
            item.mesh = mr.mesh.get();
            item.submesh = &sm;
            item.material = mat;
            item.world = world;
            item.bounds = sm.bounds.transformed(world);
            item.distance = glm::length(item.bounds.center() - cam.position);
            item.castShadows = mr.castShadows;
            item.owner = &mr.gameObject();
            m_frame.items.push_back(item);
        }
    });
    m_frame.stats.totalObjects = static_cast<uint32_t>(m_frame.items.size());

    for (uint32_t i = 0; i < m_frame.items.size(); ++i) {
        const DrawItem& it = m_frame.items[i];
        if (!cam.frustum.intersects(it.bounds)) continue;
        (it.material->shader().transparent() ? m_frame.transparent : m_frame.opaque).push_back(i);
    }
    m_frame.stats.visibleObjects = static_cast<uint32_t>(m_frame.opaque.size() + m_frame.transparent.size());

    // Opaque: group by pipeline (fewer state changes) then front-to-back
    // (early depth rejection). Transparent: back-to-front for correct blending.
    auto& items = m_frame.items;
    std::sort(m_frame.opaque.begin(), m_frame.opaque.end(), [&](uint32_t a, uint32_t b) {
        const Shader* sa = &items[a].material->shader();
        const Shader* sb = &items[b].material->shader();
        const int qa = int(sa->desc().queue), qb = int(sb->desc().queue);
        if (qa != qb) return qa < qb;
        if (sa != sb) return sa < sb;
        return items[a].distance < items[b].distance;
    });
    std::sort(m_frame.transparent.begin(), m_frame.transparent.end(),
              [&](uint32_t a, uint32_t b) { return items[a].distance > items[b].distance; });

    gatherLights(scene);
}

void Renderer::buildCascades(const glm::vec3& lightDir, uint32_t lightIndex, uint32_t slot, GpuDirectionalLight& gpu)
{
    const CameraData& cam = m_frame.camera;
    const uint32_t n = m_settings.cascadeCount;
    const float res = static_cast<float>(m_settings.directionalShadowResolution);
    const float nearP = cam.nearPlane;
    const float farP = std::min(cam.farPlane, m_settings.maxShadowDistance);
    const float lambda = m_settings.cascadeSplitLambda;
    const float casterMargin = 150.0f; // how far "behind" the cascade casters are captured

    const uint32_t baseView = slot * kMaxCascades;
    const uint32_t baseLayer = slot * n;
    const glm::vec3 up = std::abs(lightDir.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
    const float tanHalf = std::tan(cam.fovY * 0.5f);

    float prevSplit = nearP;
    for (uint32_t c = 0; c < n; ++c) {
        // "Practical split scheme": blend of logarithmic and uniform splits.
        const float p = static_cast<float>(c + 1) / static_cast<float>(n);
        const float logSplit = nearP * std::pow(farP / nearP, p);
        const float uniSplit = nearP + (farP - nearP) * p;
        const float split = lambda * logSplit + (1.0f - lambda) * uniSplit;

        // Corners of this slice of the camera frustum, in world space.
        glm::vec3 corners[8];
        int k = 0;
        for (float d : {prevSplit, split}) {
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
        m_lights->shadowViews[viewIndex] = vp;
        ShadowView sv;
        sv.viewProj = vp;
        sv.frustum = Frustum::fromMatrix(vp);
        // Directional shadow pipelines use depth clamp, so casters between the
        // sun and the cascade (beyond casterMargin, e.g. a distant mountain at
        // sunset) still land in the map at depth 0. Don't cull them by the near
        // plane.
        if (m_device.features().depthClamp) sv.frustum.planes[4] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        sv.viewIndex = viewIndex;
        sv.layer = baseLayer + c;
        sv.lightIndex = lightIndex;
        sv.texelSize = 2.0f * radius / res;
        m_frame.shadowViews.push_back(sv);

        gpu.cascadeSplits[c] = split;
        gpu.cascadeTexel[c] = 2.0f * radius / res;
        prevSplit = split;
    }
    gpu.shadow = glm::ivec4(static_cast<int>(baseView), static_cast<int>(n), static_cast<int>(baseLayer), 0);
}

void Renderer::gatherLights(Scene& scene)
{
    const CameraData& cam = m_frame.camera;

    // --- Directional lights ---------------------------------------------------
    std::vector<DirectionalLight*> dirs;
    scene.forEach<DirectionalLight>([&](DirectionalLight& l) {
        if (l.isActive() && l.intensity > 0.0f && dirs.size() < kMaxDirectionalLights) dirs.push_back(&l);
    });
    // Brightest lights get shadow maps first.
    std::sort(dirs.begin(), dirs.end(), [](auto* a, auto* b) { return a->intensity > b->intensity; });
    uint32_t dirShadowSlots = 0;
    for (uint32_t i = 0; i < dirs.size(); ++i) {
        const DirectionalLight& l = *dirs[i];
        GpuDirectionalLight& g = m_lights->directional[i];
        const glm::vec3 dir = glm::normalize(l.transform().forward());
        g.direction = glm::vec4(dir, 0.0f);
        g.color = glm::vec4(l.color * l.intensity, 0.0f);
        g.shadow = glm::ivec4(-1, 0, 0, 0);
        g.cascadeSplits = glm::vec4(0.0f);
        g.cascadeTexel = glm::vec4(0.0f);
        if (l.castShadows && dirShadowSlots < m_settings.maxShadowedDirectionalLights) {
            buildCascades(dir, i, dirShadowSlots++, g);
        }
    }

    // --- Point lights ---------------------------------------------------------
    struct Candidate {
        PointLight* light;
        float distance;
    };
    std::vector<Candidate> points;
    scene.forEach<PointLight>([&](PointLight& l) {
        if (!l.isActive() || l.intensity <= 0.0f) return;
        const glm::vec3 p = l.transform().position();
        if (!cam.frustum.intersectsSphere(p, l.range)) return; // can't affect anything we see
        points.push_back({&l, glm::length(p - cam.position)});
    });
    std::sort(points.begin(), points.end(), [](const auto& a, const auto& b) { return a.distance < b.distance; });
    if (points.size() > kMaxPointLights) points.resize(kMaxPointLights);

    const uint32_t pointViewBase = kMaxDirectionalLights * kMaxCascades;
    uint32_t cubeSlots = 0;
    for (uint32_t i = 0; i < points.size(); ++i) {
        const PointLight& l = *points[i].light;
        GpuPointLight& g = m_lights->point[i];
        const glm::vec3 pos = l.transform().position();
        g.positionRange = glm::vec4(pos, l.range);
        g.color = glm::vec4(l.color * l.intensity, 0.0f);
        g.shadow = glm::ivec4(-1, 0, 0, 0);
        g.params = glm::vec4(l.shadowBias, 0, 0, 0);

        const bool wantsShadow = l.castShadows && cubeSlots < m_settings.maxShadowedPointLights &&
                                 points[i].distance - l.range < m_settings.maxShadowDistance;
        if (!wantsShadow) continue;

        const uint32_t cube = cubeSlots++;
        const uint32_t firstView = pointViewBase + cube * 6;
        g.shadow = glm::ivec4(static_cast<int>(cube), static_cast<int>(firstView), 0, 0);
        // Standard cube-map face orientation (same as OpenGL; it also works
        // in Vulkan because both the projection and the lookup flip together).
        static const glm::vec3 dirs6[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        static const glm::vec3 ups6[6] = {{0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};
        const glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.05f, l.range);
        for (uint32_t f = 0; f < 6; ++f) {
            const glm::mat4 vp = proj * glm::lookAt(pos, pos + dirs6[f], ups6[f]);
            m_lights->shadowViews[firstView + f] = vp;
            ShadowView sv;
            sv.viewProj = vp;
            sv.frustum = Frustum::fromMatrix(vp);
            sv.viewIndex = firstView + f;
            sv.layer = cube * 6 + f;
            sv.lightIndex = i;
            sv.point = true;
            sv.lightPosition = pos;
            sv.range = l.range;
            m_frame.shadowViews.push_back(sv);
        }
    }

    m_frame.stats.directionalLights = static_cast<uint32_t>(dirs.size());
    m_frame.stats.pointLights = static_cast<uint32_t>(points.size());
    m_frame.stats.shadowViews = static_cast<uint32_t>(m_frame.shadowViews.size());
    m_frame.globals.lightCounts = glm::uvec4(dirs.size(), points.size(), m_settings.cascadeCount, 0);
}

void Renderer::fillGlobals(const Time& time)
{
    const CameraData& cam = m_frame.camera;
    FrameGlobals& g = m_frame.globals;
    const VkExtent2D ext = m_swapchain.extent();
    g.view = cam.view;
    g.proj = cam.proj;
    g.viewProj = cam.viewProj;
    g.invView = glm::inverse(cam.view);
    g.invProj = glm::inverse(cam.proj);
    g.invViewProj = glm::inverse(cam.viewProj);
    g.prevViewProj = m_frameNumber == 0 ? cam.viewProj : m_prevViewProj;
    g.cameraPosition = glm::vec4(cam.position, cam.nearPlane);
    g.cameraForward = glm::vec4(cam.forward, cam.farPlane);
    g.screenSize = glm::vec4(float(ext.width), float(ext.height), 1.0f / float(ext.width), 1.0f / float(ext.height));
    g.time = glm::vec4(time.time, time.deltaTime, float(m_frameNumber % 100000), time.realTime);
    g.ambientSky = glm::vec4(m_environment.ambientSky * m_environment.ambientIntensity, 0.0f);
    g.ambientGround = glm::vec4(m_environment.ambientGround * m_environment.ambientIntensity,
                                m_environment.groundSunFraction);
    g.lightCounts.w = m_settings.debugCascades ? 1u : 0u;
    g.shadowParams = glm::vec4(m_settings.shadowNormalBias, m_settings.shadowPcfRadius,
                               float(m_settings.directionalShadowResolution), float(m_settings.pointShadowResolution));
    g.builtinTextures = glm::uvec4(m_graph->get(m_sceneDepth).sampledIndex, m_graph->get(m_sceneColor).sampledIndex,
                                   m_graph->get(m_sceneNormal).sampledIndex, 0);
    for (uint32_t i = 0; i < kGlobalParamCount; ++i) g.globalParams[i] = m_globalParams[i];
}

// -----------------------------------------------------------------------------
// Frame
// -----------------------------------------------------------------------------

bool Renderer::render(Scene& scene, const Time& time)
{
    if (!m_initialized) return false;
    const VkExtent2D size = m_window->drawableSize();
    if (size.width == 0 || size.height == 0) return false;
    if (m_swapchainDirty && !recreateSwapchain()) return false;

    VkDevice dev = m_device.handle();
    const uint32_t slotIndex = static_cast<uint32_t>(m_frameNumber % Device::kFramesInFlight);
    FrameSlot& slot = m_slots[slotIndex];

    VK_CHECK(vkWaitForFences(dev, 1, &slot.inFlight, VK_TRUE, UINT64_MAX));
    m_device.collectGarbage(m_frameNumber);
    m_instances->collectStats(slotIndex);

    // This slot's previous frame is finished: read back its pass timings.
    if (slot.timestamps && slot.timestampsWritten && !slot.timedPasses.empty()) {
        std::vector<uint64_t> ts(slot.timedPasses.size() * 2);
        if (vkGetQueryPoolResults(dev, slot.timestamps, 0, static_cast<uint32_t>(ts.size()), ts.size() * sizeof(uint64_t),
                                  ts.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            m_graph->applyTimings(ts, slot.timedPasses, m_timestampPeriodNs);
            const float total = static_cast<float>(ts.back() - ts.front()) * m_timestampPeriodNs * 1e-6f;
            m_gpuFrameMs = m_gpuFrameMs * 0.9f + total * 0.1f;
        }
    }

    uint32_t imageIndex = 0;
    const bool headless = m_settings.headless;
    if (!headless) {
        const VkResult acquire = vkAcquireNextImageKHR(dev, m_swapchain.handle(), UINT64_MAX, slot.imageAvailable,
                                                       VK_NULL_HANDLE, &imageIndex);
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
            m_swapchainDirty = true;
            return false;
        }
        if (acquire == VK_SUBOPTIMAL_KHR) m_swapchainDirty = true;
        else VK_CHECK(acquire);
    }
    VK_CHECK(vkResetFences(dev, 1, &slot.inFlight));

    // --- CPU side: gather, cull, prepare ------------------------------------------
    m_frame.scene = &scene;
    m_frame.deltaTime = time.deltaTime;
    m_frame.time = time.time;
    m_frame.frameNumber = m_frameNumber;
    gatherScene(scene);
    fillGlobals(time);
    m_instances->prepare(slotIndex, m_frame); // also assigns ShadowView::cullView
    m_graph->prepare(m_frame);

    std::memcpy(slot.globals.mapped, &m_frame.globals, sizeof(FrameGlobals));
    std::memcpy(slot.lights.mapped, m_lights.get(), sizeof(LightBuffer));
    std::memcpy(slot.materials.mapped, m_materials.data(), sizeof(GpuMaterial) * m_materialHighWater);

    // --- Record -------------------------------------------------------------------
    VK_CHECK(vkResetCommandPool(dev, slot.pool, 0));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(slot.cmd, &begin));

    // The swapchain image's previous contents are irrelevant (UNDEFINED). Its
    // first use must wait for the acquire semaphore, which the submit below
    // waits on at COLOR_ATTACHMENT_OUTPUT, so the barrier chains from that stage.
    if (headless) {
        m_graph->setImported(m_backbuffer, m_headlessTarget.handle, m_headlessTarget.view, m_headlessTarget.format,
                             {m_headlessTarget.extent.width, m_headlessTarget.extent.height},
                             {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE});
    } else {
        m_graph->setImported(m_backbuffer, m_swapchain.image(imageIndex), m_swapchain.view(imageIndex),
                             m_swapchain.format(), m_swapchain.extent(),
                             {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_NONE});
    }

    if (slot.timestamps) vkCmdResetQueryPool(slot.cmd, slot.timestamps, 0, kMaxTimestamps);
    m_graph->setTimestampPool(slot.timestamps, kMaxTimestamps);

    RenderContext ctx;
    ctx.cmd = slot.cmd;
    ctx.renderer = this;
    ctx.graph = m_graph.get();
    ctx.frame = &m_frame;
    ctx.frameSlot = slotIndex;
    m_graph->execute(ctx);
    slot.timedPasses = m_graph->lastExecuted();
    m_instances->recordReadback(slot.cmd, slotIndex);
    slot.timestampsWritten = slot.timestamps != VK_NULL_HANDLE;
    m_graph->transition(slot.cmd, m_backbuffer, headless ? Access::TransferSrc : Access::Present);
    if (headless && !m_envCapturePath.empty() && m_frameNumber == m_envCaptureFrame) m_capturePath = m_envCapturePath;
    const bool capture = headless && !m_capturePath.empty();
    if (capture) {
        const VkExtent3D e = m_headlessTarget.extent;
        const VkDeviceSize bytes = VkDeviceSize(e.width) * e.height * 4;
        if (m_captureBuffer.size < bytes) {
            if (m_captureBuffer.handle) m_device.destroyBuffer(m_captureBuffer);
            m_captureBuffer = m_device.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::GpuToCpu,
                                                    "capture readback");
        }
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {e.width, e.height, 1};
        vkCmdCopyImageToBuffer(slot.cmd, m_headlessTarget.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               m_captureBuffer.handle, 1, &region);
    }
    VK_CHECK(vkEndCommandBuffer(slot.cmd));

    // --- Submit & present ---------------------------------------------------------
    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = slot.imageAvailable;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkSemaphore finished = headless ? VK_NULL_HANDLE : m_swapchain.renderFinished(imageIndex);
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = finished;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmdInfo.commandBuffer = slot.cmd;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = headless ? 0 : 1;
    submit.pWaitSemaphoreInfos = &wait;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;
    submit.signalSemaphoreInfoCount = headless ? 0 : 1;
    submit.pSignalSemaphoreInfos = &signal;
    VK_CHECK(vkQueueSubmit2(m_device.graphicsQueue(), 1, &submit, slot.inFlight));
    if (headless) {
        if (capture) {
            VK_CHECK(vkWaitForFences(dev, 1, &slot.inFlight, VK_TRUE, UINT64_MAX));
            vmaInvalidateAllocation(m_device.allocator(), m_captureBuffer.allocation, 0, VK_WHOLE_SIZE);
            const VkExtent3D e = m_headlessTarget.extent;
            std::vector<uint8_t> px(size_t(e.width) * e.height * 4);
            std::memcpy(px.data(), m_captureBuffer.mapped, px.size());
            const VkFormat f = m_headlessTarget.format;
            if (f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_B8G8R8A8_UNORM)
                for (size_t i = 0; i < px.size(); i += 4) std::swap(px[i], px[i + 2]);
            for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
            if (stbi_write_png(m_capturePath.c_str(), int(e.width), int(e.height), 4, px.data(), int(e.width) * 4))
                log::info("Renderer: captured frame {} to {}", m_frameNumber, m_capturePath);
            else
                log::error("Renderer: failed to write {}", m_capturePath);
            m_capturePath.clear();
        }
        m_prevViewProj = m_frame.camera.viewProj;
        ++m_frameNumber;
        return true;
    }

    const VkSwapchainKHR sc = m_swapchain.handle();
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &finished;
    present.swapchainCount = 1;
    present.pSwapchains = &sc;
    present.pImageIndices = &imageIndex;
    const VkResult pres = vkQueuePresentKHR(m_device.graphicsQueue(), &present);
    if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR) m_swapchainDirty = true;
    else VK_CHECK(pres);

    m_prevViewProj = m_frame.camera.viewProj;
    ++m_frameNumber;
    return true;
}

} // namespace ve
