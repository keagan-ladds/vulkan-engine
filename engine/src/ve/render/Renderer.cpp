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

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <initializer_list>

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
    }
    m_capture.init(m_device, m_swapchain, *m_graph, m_settings.headless); // reads VE_CAPTURE*
    m_shadowSetup.init(m_settings, m_device.features().depthClamp);
    createShadowMaps();
    writeShadowDescriptors();

    createDefaults();
    m_instances = std::make_unique<InstanceSystem>();
    m_instances->init(*this, m_settings.maxInstances, m_settings.maxVisibleInstancesPerView);
    writeInstanceDescriptors();
    m_staticBatcher.init(*this);
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
    m_staticBatcher.clear(); // (its instance groups)
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
    m_capture.shutdown();
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
// Frame
// -----------------------------------------------------------------------------

VkExtent2D Renderer::renderExtent() const
{
    if (m_capture.photoRendering()) {
        const Image& photo = m_capture.photoTarget();
        return {photo.extent.width, photo.extent.height};
    }
    return m_swapchain.extent();
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

    const bool photo = m_capture.beginFrame(); // starts a requested photo capture
    const bool offscreen = m_settings.headless || photo; // nothing to acquire or present
    const uint32_t slotIndex = static_cast<uint32_t>(m_frameNumber % Device::kFramesInFlight);
    FrameSlot& slot = m_slots[slotIndex];

    uint32_t imageIndex = 0;
    if (!acquireFrame(slot, slotIndex, offscreen, imageIndex)) return false;
    prepareFrame(scene, time, slot, slotIndex, photo);
    recordFrame(slot, slotIndex, imageIndex, photo, offscreen);
    submitFrame(slot, imageIndex, offscreen);

    m_prevViewProj = m_frame.camera.viewProjNoJitter;
    ++m_frameNumber;
    return true;
}

// Waits until the slot's previous frame is done on the GPU, then acquires the
// swapchain image to draw into (none when offscreen). False: skip this frame.
bool Renderer::acquireFrame(FrameSlot& slot, uint32_t slotIndex, bool offscreen, uint32_t& imageIndex)
{
    VkDevice dev = m_device.handle();
    const auto waitStart = std::chrono::steady_clock::now();
    VK_CHECK(vkWaitForFences(dev, 1, &slot.inFlight, VK_TRUE, UINT64_MAX));
    m_lastFenceWaitMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - waitStart).count();
    m_device.collectGarbage(m_frameNumber);
    m_instances->collectStats(slotIndex);
    readGpuTimings(slot);

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
    return true;
}

// This slot's previous frame is finished: read back its pass timings.
void Renderer::readGpuTimings(FrameSlot& slot)
{
    if (!slot.timestamps || !slot.timestampsWritten || slot.timedPasses.empty()) return;
    std::vector<uint64_t>& ts = m_timestampScratch;
    ts.resize(std::min<size_t>(slot.timedPasses.size() * 2, kMaxTimestamps));
    if (vkGetQueryPoolResults(m_device.handle(), slot.timestamps, 0, static_cast<uint32_t>(ts.size()),
                              ts.size() * sizeof(uint64_t), ts.data(), sizeof(uint64_t),
                              VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        m_graph->applyTimings(ts, slot.timedPasses, m_timestampPeriodNs);
        const float total = static_cast<float>(ts.back() - ts.front()) * m_timestampPeriodNs * 1e-6f;
        m_lastGpuFrameMs = total;
        m_gpuFrameMs = m_gpuFrameMs * 0.9f + total * 0.1f;
    }
}

// CPU side: gather, cull, let passes prepare, upload the frame's buffers.
void Renderer::prepareFrame(Scene& scene, const Time& time, FrameSlot& slot, uint32_t slotIndex, bool photo)
{
    m_frame.scene = &scene;
    m_frame.deltaTime = time.deltaTime;
    m_frame.time = time.time;
    m_frame.frameNumber = m_frameNumber;
    updateJitter(photo);
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
    uploadFrameData(slot, slotIndex);
}

// Sub-pixel camera offset: photo samples, or TAA's 8 positions.
void Renderer::updateJitter(bool photo)
{
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
    if (const uint32_t sample = photo ? m_capture.photoSample() : 0; sample > 0) { // 0 = photo warm-up
        m_jitter = glm::vec2(halton(sample, 2), halton(sample, 3)) - 0.5f;
    } else if (m_settings.temporalAA && m_taaPass->enabled) { // no jitter without the resolve
        const uint32_t k = m_frameNumber % 8u + 1u; // 8 positions
        m_jitter = glm::vec2(halton(k, 2), halton(k, 3)) - 0.5f;
    }
}

// Camera, draw items, lights and shadow views (SceneGatherer, ShadowSetup),
// then the point lights binned into clusters (LightClusterer).
void Renderer::gatherScene(Scene& scene)
{
    SceneGatherer::Inputs in;
    in.extent = renderExtent();
    in.jitter = m_jitter;
    in.skinning = m_skinning.get();
    in.fallbackMaterial = m_fallbackMaterial.get();
    in.settings = &m_settings;
    in.shadows = &m_shadowSetup;
    in.statics = &m_staticBatcher;
    const uint32_t pointLights = m_sceneGatherer.gather(scene, in, m_frame);
    m_lightClusterer.build(m_frame.camera, *m_lights, pointLights, m_settings.clusteredLights);
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

void Renderer::uploadFrameData(FrameSlot& slot, uint32_t slotIndex)
{
    std::memcpy(slot.globals.mapped, &m_frame.globals, sizeof(FrameGlobals));
    // The light buffer up to the used part of the cluster light lists.
    std::memcpy(slot.lights.mapped, m_lights.get(),
                offsetof(LightBuffer, clusterLights) + m_lightClusterer.usedWords() * sizeof(uint32_t));
    // Only the materials that changed since this slot's buffer last got them.
    std::vector<std::pair<uint32_t, uint32_t>> materialRuns; // usually empty: no allocation
    m_materialDirty.take(slotIndex, materialRuns);
    for (const auto& [first, count] : materialRuns) {
        std::memcpy(static_cast<GpuMaterial*>(slot.materials.mapped) + first, &m_materials[first], sizeof(GpuMaterial) * count);
    }
}

void Renderer::recordFrame(FrameSlot& slot, uint32_t slotIndex, uint32_t imageIndex, bool photo, bool offscreen)
{
    VK_CHECK(vkResetCommandPool(m_device.handle(), slot.pool, 0));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(slot.cmd, &begin));
    // Buffer uploads queued since the last flush (meshes, terrain nodes, skinning...) land first.
    m_device.flushUploads(slot.cmd);

    // The swapchain image's previous contents are irrelevant (UNDEFINED). Its
    // first use must wait for the acquire semaphore, which the submit below
    // waits on at COLOR_ATTACHMENT_OUTPUT, so the barrier chains from that stage.
    const bool headless = m_settings.headless;
    if (photo) {
        const Image& target = m_capture.photoTarget();
        m_graph->setImported(m_backbuffer, target.handle, target.view, target.format,
                             {target.extent.width, target.extent.height},
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
    m_capture.recordReadback(slot.cmd, headless, m_headlessTarget, m_loadingFrames);
    VK_CHECK(vkEndCommandBuffer(slot.cmd));
}

void Renderer::submitFrame(FrameSlot& slot, uint32_t imageIndex, bool offscreen)
{
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
    // Photo sample / headless capture: wait for this frame and read it.
    m_capture.endFrame(slot.inFlight, m_frameNumber);
    if (offscreen) return;

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
}

} // namespace ve
