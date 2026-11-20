#include "ve/render/Renderer.hpp"
#include "ve/render/ShaderHotReload.hpp"

#include "ve/core/Log.hpp"
#include "ve/core/Profiler.hpp"
#include "ve/core/Time.hpp"
#include "ve/core/Window.hpp"
#include "ve/render/Mesh.hpp"
#include "ve/render/Texture.hpp"
#include "ve/render/passes/BuiltinPasses.hpp"
#include "ve/render/passes/DebugLinePass.hpp"
#include "ve/scene/Components.hpp"
#include "ve/scene/Scene.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cfloat>
#include <array>
#include <initializer_list>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace ve {

namespace {

// Writes consecutive bindings of a set-0 copy: buffers or images (one each).
void writeDescriptors(VkDevice dev, VkDescriptorSet set, uint32_t firstBinding, VkDescriptorType type,
                      std::initializer_list<VkDescriptorBufferInfo> buffers,
                      std::initializer_list<VkDescriptorImageInfo> images = {})
{
    std::array<VkWriteDescriptorSet, 8> writes{};
    uint32_t n = 0;
    for (const VkDescriptorBufferInfo& b : buffers) {
        writes[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, firstBinding + n, 0, 1, type, nullptr, &b};
        ++n;
    }
    for (const VkDescriptorImageInfo& img : images) {
        writes[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, firstBinding + n, 0, 1, type, &img};
        ++n;
    }
    vkUpdateDescriptorSets(dev, n, writes.data(), 0, nullptr);
}

} // namespace

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
            // VE_CAPTURE_COUNT=n: n consecutive frames, saved as name_0.png ... name_<n-1>.png
            const char* n = std::getenv("VE_CAPTURE_COUNT");
            m_envCaptureCount = n && *n ? std::max<uint64_t>(1, std::strtoull(n, nullptr, 10)) : 1;
        }
    }
    createShadowMaps();
    writeShadowDescriptors();

    createDefaults();
    m_instances = std::make_unique<InstanceSystem>();
    m_instances->init(*this, m_settings.maxInstances, m_settings.maxVisibleInstancesPerView);
    writeInstanceDescriptors();
    m_skinning = std::make_unique<SkinningSystem>();
    m_skinning->init(*this, m_settings.maxSkinnedVertices, m_settings.maxSkinJoints);
    for (uint32_t slot = 0; slot < Device::kFramesInFlight; ++slot) {
        writeDescriptors(m_device.handle(), m_slots[slot].set, 10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                         {{m_skinning->restBuffer(), 0, VK_WHOLE_SIZE},
                          {m_skinning->weightBuffer(), 0, VK_WHOLE_SIZE},
                          {m_skinning->matrixBuffer(slot), 0, VK_WHOLE_SIZE},
                          {m_skinning->outputBuffer(), 0, VK_WHOLE_SIZE}});
    }
    createDefaultPasses();
    m_hotReload = std::make_unique<ShaderHotReload>(m_shaderDirs);
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
    if (m_skinning) m_skinning->shutdown();
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
    if (m_photo.target.handle) m_device.destroyImage(m_photo.target);
    if (m_photo.readback.handle) m_device.destroyBuffer(m_photo.readback);
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
    VkDescriptorSetLayoutBinding b[14]{};
    b[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr};
    b[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr};
    b[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr};
    b[3] = {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_ALL, nullptr};
    b[4] = {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_ALL, nullptr};
    // GPU instancing: instances, visible lists, indirect commands, groups, cull views.
    for (uint32_t i = 5; i < 10; ++i) b[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr};
    // GPU skinning: rest vertices, weights, joint matrices, skinned output (see Skinning.hpp).
    for (uint32_t i = 10; i < 14; ++i) b[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr};
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 14;
    layoutInfo.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(dev, &layoutInfo, nullptr, &m_globalSetLayout));

    const uint32_t n = Device::kFramesInFlight;
    VkDescriptorPoolSize sizes[3] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, n},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 11 * n},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * n},
    };
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = n;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(dev, &poolInfo, nullptr, &m_globalPool));

    // The single pipeline layout used by every pipeline in the engine:
    // set 0 = globals, set 1 = bindless, kPushConstantBytes (128) of push constants.
    VkDescriptorSetLayout sets[2] = {m_globalSetLayout, m_bindless.layout()};
    VkPushConstantRange push{VK_SHADER_STAGE_ALL, 0, kPushConstantBytes};
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

        writeDescriptors(dev, s.set, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, {{s.globals.handle, 0, VK_WHOLE_SIZE}});
        writeDescriptors(dev, s.set, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                         {{s.lights.handle, 0, VK_WHOLE_SIZE}, {s.materials.handle, 0, VK_WHOLE_SIZE}});
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
        writeDescriptors(m_device.handle(), s.set, 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, {},
                         {{cmp, m_graph->get(m_dirShadowMap).image.view, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL},
                          {cmp, m_graph->get(m_pointShadowMap).image.view, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL}});
    }
}

void Renderer::writeInstanceDescriptors()
{
    for (uint32_t slot = 0; slot < Device::kFramesInFlight; ++slot) {
        writeDescriptors(m_device.handle(), m_slots[slot].set, 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                         {{m_instances->instanceBuffer(), 0, VK_WHOLE_SIZE},
                          {m_instances->visibleBuffer(), 0, VK_WHOLE_SIZE},
                          {m_instances->commandBuffer(), 0, VK_WHOLE_SIZE},
                          {m_instances->groupBuffer(slot), 0, VK_WHOLE_SIZE},
                          {m_instances->viewBuffer(slot), 0, VK_WHOLE_SIZE}});
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
    m_graph->addPass<SkinningPass>();
    m_graph->addPass<InstanceCullPass>();
    m_graph->addPass<ShadowPass>();
    m_graph->addPass<OpaquePass>();
    m_graph->addPass<TransparentPass>();
    m_graph->addPass<HiZPass>();
    m_graph->addPass<VelocityPass>();
    m_taaPass = &m_graph->addPass<TemporalAAPass>();
    m_graph->addPass<CompositePass>();
    m_graph->addPass<DebugLinePass>(); // ve::debug lines over the finished image
}

void Renderer::applySettings()
{
    m_device.waitIdle();
    createShadowMaps();
    writeShadowDescriptors();
    ++m_contentEpoch;
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
    auto shader = std::make_shared<Shader>(*this, desc);
    std::erase_if(m_shaders, [](const std::weak_ptr<Shader>& w) { return w.expired(); });
    m_shaders.push_back(shader);
    return shader;
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

PipelineHandle::operator VkPipeline() const
{
    return m_renderer ? m_renderer->pipeline(*this) : VK_NULL_HANDLE;
}

VkPipeline Renderer::pipeline(const PipelineHandle& h) const
{
    return h.m_id != 0 && h.m_id <= m_rawPipelines.size() ? m_rawPipelines[h.m_id - 1].pipeline : VK_NULL_HANDLE;
}

PipelineHandle Renderer::createFullscreenPipeline(const FullscreenPipelineDesc& desc)
{
    return addRawPipeline({buildFullscreenPipeline(desc), false, desc, {}});
}

PipelineHandle Renderer::createComputePipeline(const std::string& shaderName)
{
    return addRawPipeline({buildComputePipeline(shaderName), true, {}, shaderName});
}

PipelineHandle Renderer::addRawPipeline(RawPipeline raw)
{
    PipelineHandle h;
    h.m_renderer = this;
    if (!m_freeRawPipelines.empty()) {
        h.m_id = m_freeRawPipelines.back();
        m_freeRawPipelines.pop_back();
        m_rawPipelines[h.m_id - 1] = std::move(raw);
    } else {
        m_rawPipelines.push_back(std::move(raw));
        h.m_id = static_cast<uint32_t>(m_rawPipelines.size());
    }
    return h;
}

void Renderer::destroyPipeline(PipelineHandle& h)
{
    if (h.m_renderer == this && h.m_id != 0 && h.m_id <= m_rawPipelines.size()) {
        RawPipeline& raw = m_rawPipelines[h.m_id - 1];
        destroyPipeline(raw.pipeline);
        raw = RawPipeline{};
        m_freeRawPipelines.push_back(h.m_id);
    }
    h = PipelineHandle{};
}

void Renderer::reloadShaders(const std::vector<std::string>& names)
{
    auto has = [&](const std::string& n) { return !n.empty() && std::find(names.begin(), names.end(), n) != names.end(); };
    uint32_t rebuilt = 0;
    for (RawPipeline& raw : m_rawPipelines) {
        if (!raw.pipeline) continue;
        const bool hit = raw.compute ? has(raw.computeShader) : has(raw.fullscreen.fragment) || has("fullscreen.vert");
        if (!hit) continue;
        try {
            VkPipeline fresh = raw.compute ? buildComputePipeline(raw.computeShader) : buildFullscreenPipeline(raw.fullscreen);
            destroyPipeline(raw.pipeline);
            raw.pipeline = fresh;
            ++rebuilt;
        } catch (const std::exception& e) {
            log::error("Shader hot-reload: keeping the old pipeline: {}", e.what());
        }
    }
    for (const auto& w : m_shaders) {
        const auto shader = w.lock();
        if (!shader) continue;
        const ShaderDesc& d = shader->desc();
        if (has(d.vertex) || has(d.fragment) || has(d.shadowVertex) || has(d.shadowFragment) || has(d.shadowPointFragment)) {
            shader->invalidate(); // rebuilt on next use
            ++rebuilt;
        }
    }
    ++m_contentEpoch;
    log::info("Shader hot-reload: {} pipeline(s) rebuilt", rebuilt);
}

VkPipeline Renderer::buildFullscreenPipeline(const FullscreenPipelineDesc& desc)
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

VkPipeline Renderer::buildComputePipeline(const std::string& shaderName)
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
    // Both frame slots are rewritten, including one the GPU may still be
    // reading: wait for it first (load-time path: impostor baking).
    m_device.waitIdle();
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
            item.mesh->bind(cmd);
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
    const VkExtent2D ext = renderExtent();
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
    cam.viewProjNoJitter = cam.proj * cam.view;
    // Sub-pixel jitter (TAA, photo capture): shift the image by a fraction of a pixel.
    cam.proj[2][0] += m_jitter.x * 2.0f / float(std::max(1u, ext.width));
    cam.proj[2][1] += m_jitter.y * 2.0f / float(std::max(1u, ext.height));
    cam.viewProj = cam.proj * cam.view;
    cam.frustum = Frustum::fromMatrix(cam.viewProj);
}

void Renderer::gatherScene(Scene& scene)
{
    m_sceneTop = -FLT_MAX; // highest drawn geometry (shadow caster reach, buildCascades)
    m_frame.items.clear();
    m_frame.opaque.clear();
    m_frame.transparent.clear();
    m_frame.moving.clear();
    m_frame.shadowViews.clear();
    m_frame.stats = {};

    setupCamera(scene);
    const CameraData& cam = m_frame.camera;

    // Skinned meshes: joint matrices for this frame's skinning pass (and their bounds).
    // Their vertices are rewritten on the GPU every frame, so their draw items
    // are flagged `deforming` (the point-shadow face cache must not trust them).
    m_deforming.clear();
    for (const auto& gather : m_skinning->gatherers()) gather(scene, *m_skinning, m_deforming);
    std::sort(m_deforming.begin(), m_deforming.end());
    // Last frame's world matrices (motion vectors for moving objects). One
    // persistent map, stamped with the frame that last saw each renderer:
    // no per-frame rebuild, only new renderers allocate.
    const uint64_t stamp = ++m_prevWorldStamp;
    size_t seen = 0;
    scene.forEach<MeshRenderer>([&](MeshRenderer& mr) {
        if (!mr.isActive() || !mr.visible || !mr.mesh) return;
        const glm::mat4& world = mr.transform().worldMatrix();
        const auto prev = m_prevWorlds.try_emplace(&mr, PrevWorld{world, stamp}).first; // new: prev = world
        const glm::mat4 prevWorld = prev->second.world;
        prev->second = {world, stamp};
        ++seen;
        const bool deforming = !m_deforming.empty() && std::binary_search(m_deforming.begin(), m_deforming.end(), &mr);
        for (const Submesh& sm : mr.mesh->submeshes()) {
            Material* mat = mr.material(sm.materialSlot);
            if (!mat) mat = m_fallbackMaterial.get();
            DrawItem item;
            item.mesh = mr.mesh.get();
            item.submesh = &sm;
            item.material = mat;
            item.world = world;
            item.prevWorld = prevWorld;
            item.bounds = sm.bounds.transformed(world);
            item.distance = glm::length(item.bounds.center() - cam.position);
            item.castShadows = mr.castShadows;
            item.deforming = deforming;
            item.owner = &mr.gameObject();
            m_sceneTop = std::max(m_sceneTop, item.bounds.max.y);
            m_frame.items.push_back(item);
        }
    });
    // Forget renderers not drawn this frame (destroyed or hidden): a new
    // renderer at a recycled address must not inherit a stale matrix.
    if (m_prevWorlds.size() != seen) std::erase_if(m_prevWorlds, [&](const auto& kv) { return kv.second.stamp != stamp; });
    m_frame.stats.totalObjects = static_cast<uint32_t>(m_frame.items.size());

    for (uint32_t i = 0; i < m_frame.items.size(); ++i) {
        const DrawItem& it = m_frame.items[i];
        if (!cam.frustum.intersects(it.bounds)) continue;
        const bool transparent = it.material->shader().transparent();
        (transparent ? m_frame.transparent : m_frame.opaque).push_back(i);
        if (!transparent && it.prevWorld != it.world) m_frame.moving.push_back(i);
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

// The direction directional shadow maps are rendered from: the light's,
// snapped to shadowDirectionStep-degree steps of azimuth and elevation. A sun
// that moves every frame would otherwise re-rasterise every shadow edge from a
// slightly different angle each frame, and the edges' stair steps crawl
// (the whole scene seems to shimmer). Snapped, shadows stand still and move in
// small steps; lighting keeps the exact direction.
glm::vec3 Renderer::shadowDirection(const glm::vec3& dir) const
{
    const float step = glm::radians(m_settings.shadowDirectionStep);
    if (step <= 0.0f) return dir;
    float elevation = std::asin(std::clamp(dir.y, -1.0f, 1.0f));
    if (std::abs(dir.y) > 0.9995f) return dir; // overhead: azimuth is meaningless
    float azimuth = std::atan2(dir.z, dir.x);
    elevation = std::round(elevation / step) * step;
    azimuth = std::round(azimuth / step) * step;
    return {std::cos(elevation) * std::cos(azimuth), std::sin(elevation), std::cos(elevation) * std::sin(azimuth)};
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
        m_lights->shadowViews[viewIndex] = vp;
        ShadowView sv;
        sv.viewProj = vp;
        sv.frustum = Frustum::fromMatrix(vp);
        // Directional shadow pipelines use depth clamp, so casters between the
        // sun and the cascade (beyond casterMargin, e.g. a distant mountain at
        // sunset) still land in the map at depth 0. Don't cull them by the near
        // plane.
        if (m_device.features().depthClamp) {
            // ...but only as far as a caster could still shadow this cascade:
            // going towards the sun, the ray from a receiver climbs sin(elevation)
            // per metre, so nothing further than (scene top - cascade bottom) /
            // sin(elevation) can reach down into it. With a low sun over a big
            // world this skips everything beyond the hills that matter.
            const float sinElevation = -lightDir.y;
            const float top = (m_sceneTop > -FLT_MAX ? m_sceneTop : centroid.y) + m_settings.shadowInstanceHeight;
            const float rise = std::max(top - (centroid.y - radius), 0.0f);
            // Near sunset that bound grows without limit, and everything towards
            // the sun is pancaked into every cascade (overdraw across whole
            // maps); shadowCasterReach caps it.
            float reach = std::max(m_settings.shadowCasterReach, casterMargin);
            if (sinElevation > 0.02f) reach = std::min(reach, std::max(rise / sinElevation, casterMargin));
            const glm::vec3 p0 = centroid - lightDir * (radius + reach);
            sv.frustum.planes[4] = glm::vec4(lightDir, -glm::dot(lightDir, p0));
        }
        sv.viewIndex = viewIndex;
        sv.layer = baseLayer + c;
        sv.lightIndex = lightIndex;
        sv.texelSize = 2.0f * radius / res;
        m_frame.shadowViews.push_back(sv);

        gpu.cascadeSplits[c] = split;
        gpu.cascadeTexel[c] = 2.0f * radius / res;
        prevBegin = c == 0 ? 0.0f : prevSplit;
        prevSplit = split;
    }
    gpu.shadow = glm::ivec4(static_cast<int>(baseView), static_cast<int>(n), static_cast<int>(baseLayer), 0);
}

// Bins the point lights into the camera's clusters (on the CPU: there are at
// most a few hundred, already frustum culled). Each light covers the screen
// tiles of its bounding box's projection in the depth slices it spans. If the
// index list overflows, the clusters are flagged invalid and shaders fall back
// to looping over every light.
void Renderer::buildLightClusters(uint32_t count)
{
    const CameraData& cam = m_frame.camera;
    LightBuffer& lb = *m_lights;
    const float z0 = std::max(cam.nearPlane, 0.5f), z1 = std::max(cam.farPlane, z0 * 2.0f);
    const float scale = float(kClusterZ) / std::log(z1 / z0);
    lb.clusterParams = glm::vec4(scale, -std::log(z0) * scale, m_settings.clusteredLights ? 1.0f : 0.0f, 0.0f);
    m_clusterWords = 0;
    if (!m_settings.clusteredLights) return;
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

void Renderer::gatherLights(Scene& scene)
{
    const CameraData& cam = m_frame.camera;

    // --- Directional lights ---------------------------------------------------
    std::vector<DirectionalLight*>& dirs = m_dirScratch;
    dirs.clear();
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
            buildCascades(shadowDirection(dir), i, dirShadowSlots++, g);
        }
    }

    // --- Point lights ---------------------------------------------------------
    std::vector<PointCandidate>& points = m_pointScratch;
    points.clear();
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
    buildLightClusters(uint32_t(points.size()));
}

void Renderer::fillGlobals(const Time& time)
{
    const CameraData& cam = m_frame.camera;
    FrameGlobals& g = m_frame.globals;
    const VkExtent2D ext = renderExtent();
    g.view = cam.view;
    g.proj = cam.proj;
    g.viewProj = cam.viewProj;
    g.invView = glm::inverse(cam.view);
    g.invProj = glm::inverse(cam.proj);
    g.invViewProj = glm::inverse(cam.viewProj);
    // Unjittered: reprojection (TAA, temporal accumulators) maps to where a
    // point was, not to last frame's jittered sample of it.
    g.prevViewProj = m_frameNumber == 0 ? cam.viewProjNoJitter : m_prevViewProj;
    // How far the image moved: adding j to proj[2] moves NDC by -j * proj[2][3]
    // (= -j, as clip.w = -z), so content shifts by minus the jitter.
    g.jitter = glm::vec4(-m_jitter.x / float(ext.width), -m_jitter.y / float(ext.height), 0.0f, 0.0f);
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
    g.builtinTextures = glm::uvec4(m_graph->sampled(m_sceneDepth), m_graph->sampled(m_sceneColor),
                                   m_graph->sampled(m_sceneNormal), 0);
    for (uint32_t i = 0; i < kGlobalParamCount; ++i) g.globalParams[i] = m_globalParams[i];
}

// -----------------------------------------------------------------------------
// Frame
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Photo capture
// -----------------------------------------------------------------------------

VkExtent2D Renderer::renderExtent() const
{
    if (m_photo.active && m_photo.started) return {m_photo.target.extent.width, m_photo.target.extent.height};
    return m_swapchain.extent();
}

void Renderer::beginPhotoCapture(const PhotoCaptureSettings& settings)
{
    if (m_photo.active) return;
    m_photo.settings = settings;
    m_photo.settings.samples = std::max(settings.samples, 1u);
    m_photo.active = true;
    m_photo.started = false;
}

float Renderer::photoCaptureProgress() const
{
    if (!m_photo.active) return 0.0f;
    const float total = float(m_photo.settings.warmupFrames + m_photo.settings.samples);
    return std::min(float(m_photo.frame) / total, 1.0f);
}

void Renderer::photoBegin()
{
    PhotoCaptureSettings& s = m_photo.settings;
    const VkExtent2D win = m_swapchain.extent();
    const uint32_t limit = m_device.properties().limits.maxImageDimension2D;
    const uint32_t w = std::clamp(s.width ? s.width : win.width, 16u, limit);
    const uint32_t h = std::clamp(s.height ? s.height : win.height, 16u, limit);
    m_device.waitIdle();
    m_photo.target = m_device.createImage({.format = m_swapchain.format(),
                                           .extent = {w, h, 1},
                                           .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                           .debugName = "photo target"});
    m_photo.readback = m_device.createBuffer(VkDeviceSize(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                             MemoryUsage::GpuToCpu, "photo readback");
    m_photo.accum.assign(size_t(w) * h * 3, 0.0f);
    m_photo.frame = 0;
    m_photo.started = true;
    m_graph->resize({w, h}); // screen-sized textures follow the photo's size
    log::info("Renderer: photo {}x{}, {} samples (+{} warm-up frames)", w, h, s.samples, s.warmupFrames);
}

void Renderer::photoAccumulate()
{
    const uint32_t frame = m_photo.frame++;
    if (frame < m_photo.settings.warmupFrames) return;
    vmaInvalidateAllocation(m_device.allocator(), m_photo.readback.allocation, 0, VK_WHOLE_SIZE);
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

void Renderer::photoFinish()
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
    m_device.waitIdle();
    m_device.destroyImage(m_photo.target);
    m_device.destroyBuffer(m_photo.readback);
    m_photo.accum = {};
    m_photo.active = m_photo.started = false;
    m_graph->resize(m_swapchain.extent());
}

void Renderer::saveCapture()
{
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

bool Renderer::render(Scene& scene, const Time& time)
{
    if (!m_initialized) return false;
    const VkExtent2D size = m_window->drawableSize();
    if (size.width == 0 || size.height == 0) return false;
    if (m_swapchainDirty && !recreateSwapchain()) return false;
    if (m_hotReload) {
        if (auto changed = m_hotReload->poll(); !changed.empty()) reloadShaders(changed);
    }

    const bool photo = m_photo.active;
    if (photo && !m_photo.started) photoBegin();

    VkDevice dev = m_device.handle();
    const uint32_t slotIndex = static_cast<uint32_t>(m_frameNumber % Device::kFramesInFlight);
    FrameSlot& slot = m_slots[slotIndex];

    const auto waitStart = std::chrono::steady_clock::now();
    VK_CHECK(vkWaitForFences(dev, 1, &slot.inFlight, VK_TRUE, UINT64_MAX));
    m_lastFenceWaitMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - waitStart).count();
    m_device.collectGarbage(m_frameNumber);
    m_instances->collectStats(slotIndex);

    // This slot's previous frame is finished: read back its pass timings.
    if (slot.timestamps && slot.timestampsWritten && !slot.timedPasses.empty()) {
        std::vector<uint64_t>& ts = m_timestampScratch;
        ts.resize(std::min<size_t>(slot.timedPasses.size() * 2, kMaxTimestamps));
        if (vkGetQueryPoolResults(dev, slot.timestamps, 0, static_cast<uint32_t>(ts.size()), ts.size() * sizeof(uint64_t),
                                  ts.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            m_graph->applyTimings(ts, slot.timedPasses, m_timestampPeriodNs);
            const float total = static_cast<float>(ts.back() - ts.front()) * m_timestampPeriodNs * 1e-6f;
            m_lastGpuFrameMs = total;
            m_gpuFrameMs = m_gpuFrameMs * 0.9f + total * 0.1f;
        }
    }

    uint32_t imageIndex = 0;
    const bool headless = m_settings.headless;
    const bool offscreen = headless || photo; // nothing to acquire or present
    if (!offscreen) {
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
    // Halton (2, 3): evenly spread sub-pixel offsets for any sample count.
    auto halton = [](uint32_t i, uint32_t base) {
        float f = 1.0f, r = 0.0f;
        for (; i > 0; i /= base) {
            f /= float(base);
            r += f * float(i % base);
        }
        return r;
    };
    m_jitter = glm::vec2(0.0f);
    if (photo && m_photo.frame >= m_photo.settings.warmupFrames) {
        const uint32_t k = m_photo.frame - m_photo.settings.warmupFrames + 1;
        m_jitter = glm::vec2(halton(k, 2), halton(k, 3)) - 0.5f;
    } else if (m_settings.temporalAA && m_taaPass->enabled) { // no jitter without the resolve
        const uint32_t k = m_frameNumber % 8u + 1u; // 8 positions
        m_jitter = glm::vec2(halton(k, 2), halton(k, 3)) - 0.5f;
    }
    m_graph->maxStage = photo ? int(RenderStage::Overlay) : (1 << 30);
    m_graph->loadingFrame = m_loadingFrames;
    m_skinning->beginFrame(slotIndex);
    {
        VE_PROFILE("Gather scene");
        gatherScene(scene);
        fillGlobals(time);
    }
    {
        VE_PROFILE("Instances");
        m_instances->prepare(slotIndex, m_frame); // also assigns ShadowView::cullView
    }
    {
        VE_PROFILE("Pass prepare");
        m_graph->prepare(m_frame);
    }

    std::memcpy(slot.globals.mapped, &m_frame.globals, sizeof(FrameGlobals));
    // The light buffer up to the used part of the cluster light lists.
    std::memcpy(slot.lights.mapped, m_lights.get(), offsetof(LightBuffer, clusterLights) + m_clusterWords * sizeof(uint32_t));
    std::memcpy(slot.materials.mapped, m_materials.data(), sizeof(GpuMaterial) * m_materialHighWater);

    // --- Record -------------------------------------------------------------------
    VK_CHECK(vkResetCommandPool(dev, slot.pool, 0));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(slot.cmd, &begin));

    // The swapchain image's previous contents are irrelevant (UNDEFINED). Its
    // first use must wait for the acquire semaphore, which the submit below
    // waits on at COLOR_ATTACHMENT_OUTPUT, so the barrier chains from that stage.
    if (photo) {
        m_graph->setImported(m_backbuffer, m_photo.target.handle, m_photo.target.view, m_photo.target.format,
                             {m_photo.target.extent.width, m_photo.target.extent.height},
                             {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE});
    } else if (headless) {
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
    {
        VE_PROFILE("Record commands");
        m_graph->execute(ctx);
    }
    slot.timedPasses = m_graph->lastTimed();
    m_instances->recordReadback(slot.cmd, slotIndex);
    slot.timestampsWritten = slot.timestamps != VK_NULL_HANDLE;
    m_graph->transition(slot.cmd, m_backbuffer, offscreen ? Access::TransferSrc : Access::Present);
    // Read the finished frame back: photo accumulation, or a headless capture.
    auto readback = [&](const Image& image, const Buffer& buffer) {
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = image.extent;
        vkCmdCopyImageToBuffer(slot.cmd, image.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer.handle, 1, &region);
    };
    if (photo) readback(m_photo.target, m_photo.readback);
    // VE_CAPTURE_FRAME counts world frames (loading-screen frames don't count).
    const uint64_t worldFrame = m_worldFrames;
    if (!m_loadingFrames) ++m_worldFrames;
    if (headless && !m_loadingFrames && !m_envCapturePath.empty() && worldFrame >= m_envCaptureFrame &&
        worldFrame < m_envCaptureFrame + m_envCaptureCount) {
        m_capturePath = m_envCapturePath;
        if (m_envCaptureCount > 1) {
            const std::filesystem::path p(m_envCapturePath);
            m_capturePath = (p.parent_path() / (p.stem().string() + "_" + std::to_string(worldFrame - m_envCaptureFrame) +
                                                p.extension().string())).string();
        }
    }
    const bool capture = headless && !photo && !m_capturePath.empty();
    if (capture) {
        const VkExtent3D e = m_headlessTarget.extent;
        const VkDeviceSize bytes = VkDeviceSize(e.width) * e.height * 4;
        if (m_captureBuffer.size < bytes) {
            if (m_captureBuffer.handle) m_device.destroyBuffer(m_captureBuffer);
            m_captureBuffer = m_device.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::GpuToCpu,
                                                    "capture readback");
        }
        readback(m_headlessTarget, m_captureBuffer);
    }
    VK_CHECK(vkEndCommandBuffer(slot.cmd));

    // --- Submit & present ---------------------------------------------------------
    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = slot.imageAvailable;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkSemaphore finished = offscreen ? VK_NULL_HANDLE : m_swapchain.renderFinished(imageIndex);
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = finished;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmdInfo.commandBuffer = slot.cmd;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = offscreen ? 0 : 1;
    submit.pWaitSemaphoreInfos = &wait;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;
    submit.signalSemaphoreInfoCount = offscreen ? 0 : 1;
    submit.pSignalSemaphoreInfos = &signal;
    const auto submitStart = std::chrono::steady_clock::now();
    VK_CHECK(vkQueueSubmit2(m_device.graphicsQueue(), 1, &submit, slot.inFlight));
    m_lastSubmitMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - submitStart).count();
    if (photo) {
        VK_CHECK(vkWaitForFences(dev, 1, &slot.inFlight, VK_TRUE, UINT64_MAX));
        photoAccumulate(); // may finish the photo
    }
    if (offscreen) {
        if (capture) {
            VK_CHECK(vkWaitForFences(dev, 1, &slot.inFlight, VK_TRUE, UINT64_MAX));
            saveCapture();
        }
        m_prevViewProj = m_frame.camera.viewProjNoJitter;
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
    const auto presentStart = std::chrono::steady_clock::now();
    const VkResult pres = vkQueuePresentKHR(m_device.graphicsQueue(), &present);
    m_lastSubmitMs += std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - presentStart).count();
    if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR) m_swapchainDirty = true;
    else VK_CHECK(pres);

    m_prevViewProj = m_frame.camera.viewProjNoJitter;
    ++m_frameNumber;
    return true;
}

} // namespace ve
