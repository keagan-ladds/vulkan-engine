#pragma once

// =============================================================================
// Renderer - turns a Scene into pixels.
//
// Owns the GPU (Device), swapchain, bindless table, samplers, the global
// descriptor set + pipeline layout shared by all shaders, per-frame buffers,
// built-in shaders and the RenderGraph. Each frame (render()):
//
//   1. acquire: wait for the frame slot, acquire a swapchain image
//   2. prepare: gather camera / draw items / lights from the scene, cull,
//      sort (SceneGatherer; static renderers are batched once into instance
//      groups by StaticBatcher), shadow cascades + cube faces (ShadowSetup),
//      light + decal clusters (LightClusterer); let passes prepare(), upload
//      FrameGlobals/LightBuffer/DecalBuffer/MaterialBuffer
//   3. record the RenderGraph (FrameCapture reads the image back if asked)
//   4. submit, present
//
// The per-frame CPU work lives in those focused parts (members of Renderer);
// Renderer itself keeps the frame loop, the pipeline / material / shader
// registries and the GPU objects shared by every pass.
//
// See docs/01-architecture.md §5 for the full picture.
// =============================================================================

#include "ve/gfx/Bindless.hpp"
#include "ve/gfx/Device.hpp"
#include "ve/gfx/Samplers.hpp"
#include "ve/gfx/Swapchain.hpp"
#include "ve/render/Decals.hpp"
#include "ve/render/FrameCapture.hpp"
#include "ve/render/FrameData.hpp"
#include "ve/render/Instancing.hpp"
#include "ve/render/LightClusterer.hpp"
#include "ve/render/SceneGatherer.hpp"
#include "ve/render/ShadowSetup.hpp"
#include "ve/render/StaticBatcher.hpp"
#include "ve/render/Skinning.hpp"
#include "ve/render/Material.hpp"
#include "ve/core/Event.hpp"
#include "ve/render/PipelineHandle.hpp"
#include "ve/render/ShaderHotReload.hpp"
#include "ve/render/RenderGraph.hpp"
#include "ve/render/RendererSettings.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <vector>

namespace ve {

struct CompileEvent {
    enum class Type { Started, Finished, Failed };
    enum class What { Pipeline, Shader };
    Type type = Type::Started;
    What what = What::Pipeline;
    std::string name;          // "standard.frag (Forward)", "bloom_up.frag"
    float milliseconds = 0.0f; // Finished / Failed: how long it took
    uint32_t pending = 0;      // compiles of this kind still running after this event
    std::string error;         // Failed
};

struct CompileStatus {
    uint32_t pipelinesPending = 0;
    uint32_t shadersPending = 0;    // hot-reload GLSL compiles
    uint64_t pipelinesCompiled = 0; // since start
    bool busy() const { return pipelinesPending + shadersPending > 0; }
};


class Renderer;
class Window;
class Scene;
class Texture;
class TemporalAAPass;
class Mesh;
struct Time;


// Hemispherical ambient light. Effects (e.g. vfx::AtmospherePass) usually
// drive this each frame from the sky.
struct Environment {
    glm::vec3 ambientSky{0.35f, 0.42f, 0.55f};
    glm::vec3 ambientGround{0.18f, 0.15f, 0.12f};
    float ambientIntensity = 1.0f;
    // Fraction of ambientGround that is sunlight bounced off the ground. Shaders
    // scale that part by sun visibility (veAmbientGround), so objects don't get
    // a warm "sunlit ground" glow once the sun is behind terrain.
    float groundSunFraction = 0.0f;
};

enum class BuiltinShader { Standard, StandardCutout, StandardTransparent, Unlit, Impostor, Count };

// Something that draws its own geometry into the directional shadow cascades
// (e.g. GrassPass). ShadowPass calls drawShadow() for every directional
// cascade while that cascade's depth target is bound, after the scene's
// meshes and instances. Register with Renderer::addShadowCaster.
class ShadowCaster {
public:
    virtual ~ShadowCaster() = default;
    virtual void drawShadow(RenderContext& ctx, const ShadowView& view) = 0;
};

class Renderer {
public:
    Renderer() = default;
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void init(Window& window, const RendererSettings& settings, bool validation,
              std::vector<std::filesystem::path> shaderDirectories);
    void shutdown();

    // Renders one frame of `scene` (returns false if nothing was drawn, e.g. minimized).
    bool render(Scene& scene, const Time& time);
    void onResize() { m_swapchainDirty = true; }

    // --- Access ---------------------------------------------------------------
    Device& device() { return m_device; }
    Bindless& bindless() { return m_bindless; }
    const Samplers& samplers() const { return m_samplers; }
    RenderGraph& graph() { return *m_graph; }
    const Swapchain& swapchain() const { return m_swapchain; }
    VkPipelineLayout pipelineLayout() const { return m_pipelineLayout; }
    const FrameData& frameData() const { return m_frame; }
    const RenderStats& stats() const { return m_frame.stats; }
    float gpuFrameTimeMs() const { return m_gpuFrameMs; }
    float lastGpuFrameTimeMs() const { return m_lastGpuFrameMs; } // raw, latest read back
    float lastFenceWaitMs() const { return m_lastFenceWaitMs; }  // CPU time spent waiting for the GPU in render()
    float lastSubmitMs() const { return m_lastSubmitMs; }        // CPU time inside queue submit + present (driver)
    // Bumped whenever cached GPU content may be stale (shadow maps recreated,
    // shaders hot-reloaded). Passes that keep results across frames compare it.
    uint64_t contentEpoch() const { return m_contentEpoch; }
    Window& window() { return *m_window; }

    RendererSettings& settings() { return m_settings; }
    // Call after changing shadow counts/resolutions/vsync in settings().
    void applySettings();

    Environment& environment() { return m_environment; }
    // Free-form vec4 globals visible to every shader as frame.globalParams[i].
    void setGlobalParam(uint32_t index, const glm::vec4& value);
    glm::vec4 globalParam(uint32_t index) const;

    // --- Formats used by the built-in targets ----------------------------------
    static constexpr VkFormat kSceneColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    // Thin G-buffer written by opaque geometry: xyz = world normal, w = roughness
    // (all zero where nothing was drawn). Used by screen-space effects (SSR, SSGI).
    static constexpr VkFormat kSceneNormalFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
    static constexpr VkFormat kShadowFormat = VK_FORMAT_D32_SFLOAT;

    // --- Resources ----------------------------------------------------------------
    std::shared_ptr<Shader> createShader(const ShaderDesc& desc);
    std::shared_ptr<Shader> builtinShader(BuiltinShader which) const { return m_builtinShaders[size_t(which)]; }
    std::shared_ptr<Material> createMaterial(std::shared_ptr<Shader> shader);
    SkinningSystem& skinning() { return *m_skinning; }
    // Loading screens (Engine::beginLoading): frames draw only the UI over a
    // cleared screen, plus the passes with runsWhileLoading.
    void setLoadingFrames(bool on, const glm::vec4& clearColor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f))
    {
        m_loadingFrames = on;
        m_loadingClear = clearColor;
    }
    bool loadingFrames() const { return m_loadingFrames; }
    const glm::vec4& loadingClearColor() const { return m_loadingClear; }
    // Thousands of copies of a mesh, culled and drawn on the GPU (see Instancing.hpp).
    std::shared_ptr<InstanceGroup> createInstanceGroup(InstanceGroupDesc desc, uint32_t capacity);
    InstanceSystem& instances() { return *m_instances; }
    // Static MeshRenderers batched into instance groups (RendererSettings::staticBatching).
    StaticBatcher& staticBatcher() { return m_staticBatcher; }
    // Fire-and-forget decals (bullet holes, footprints): see Decals.hpp.
    // Placed decals are DecalProjector components.
    DecalSystem& decals() { return m_decals; }
    void addShadowCaster(ShadowCaster* caster) { m_shadowCasters.push_back(caster); }
    void removeShadowCaster(ShadowCaster* caster) { std::erase(m_shadowCasters, caster); }
    const std::vector<ShadowCaster*>& shadowCasters() const { return m_shadowCasters; }
    Material* fallbackMaterial() const { return m_fallbackMaterial.get(); }
    std::shared_ptr<StandardMaterial> createStandardMaterial(StandardMaterial::Mode mode = StandardMaterial::Mode::Opaque);
    const std::shared_ptr<Texture>& whiteTexture() const { return m_white; }
    const std::shared_ptr<Texture>& blackTexture() const { return m_black; }
    const std::shared_ptr<Texture>& flatNormalTexture() const { return m_flatNormal; }

    // Finds `<name>.spv` in the shader directories and creates a module.
    // The caller destroys it (vkDestroyShaderModule) after building pipelines.
    VkShaderModule loadShader(const std::string& name);
    std::filesystem::path findShader(const std::string& name) const;

    // Builds a pipeline that draws a full-screen triangle with `fragment`
    // (uses the built-in fullscreen.vert). Optional depth test against a
    // read-only depth attachment (e.g. sky drawn only where depth == far).
    struct FullscreenPipelineDesc {
        std::string fragment;
        std::vector<VkFormat> colorFormats;
        BlendMode blend = BlendMode::Opaque;
        VkFormat depthFormat = VK_FORMAT_UNDEFINED;
        VkCompareOp depthCompare = VK_COMPARE_OP_ALWAYS;
    };
    PipelineHandle createFullscreenPipeline(const FullscreenPipelineDesc& desc);
    // Compute pipeline using the global layout.
    PipelineHandle createComputePipeline(const std::string& shaderName);
    // Destroys a pipeline once the GPU is done with it.
    void destroyPipeline(VkPipeline pipeline);
    void destroyPipeline(PipelineHandle& pipeline); // also resets the handle
    VkPipeline pipeline(const PipelineHandle& h) const;

    // Rebuilds every pipeline that uses one of these shaders (e.g.
    // "bloom_up.frag"); called by the shader hot-reload watcher. Pipelines
    // whose rebuild fails keep the old version.
    void reloadShaders(const std::vector<std::string>& names);
    ShaderHotReload* shaderHotReload() { return m_hotReload.get(); }

    // Draws `items[indices]` with each material's pipeline for `kind`.
    // Used by the built-in passes; handy for custom geometry passes too.
    void drawItems(RenderContext& ctx, const std::vector<uint32_t>& indices, PassKind kind,
                   uint32_t viewIndex = 0, uint32_t lightIndex = 0, uint32_t flags = 0);

    // --- Engine internal ------------------------------------------------------------
    VkPipeline buildMeshPipeline(const ShaderDesc& desc, PassKind kind);

    // ---- Compiles (for "compiling shaders..." UI) ----
    // Pipelines: a shader's first use per pass kind, built by the driver
    // (async: RendererSettings::asyncPipelines). Shaders: GLSL recompiled by
    // shader hot-reload (development builds).
    CompileStatus compileStatus() const;
    // Started / Finished / Failed per compile, on the main thread (async ones
    // finish at the start of a later frame).
    Event<const CompileEvent&>& compileEvents() { return m_compileEvents; }
    // Blocks until every pipeline compile in flight is done (screenshots, tests).
    void waitForPipelines();
    // (Shader's side: starts building one of its pipelines; drops its jobs.)
    void requestPipeline(Shader& shader, PassKind kind);
    void detachPipelineJobs(Shader& shader);
    uint32_t allocateMaterialSlot();
    void freeMaterialSlot(uint32_t index);
    GpuMaterial& materialData(uint32_t index) { return m_materials[index]; }
    void markMaterialDirty(uint32_t index) { m_materialDirty.mark(index); }
    void bindGlobalSets(VkCommandBuffer cmd);
    // Copies all material parameters to the GPU now (normally done per frame);
    // for GPU work at load time that reads materials, e.g. impostor baking.
    void uploadMaterialsNow();
    uint64_t frameNumber() const { return m_frameNumber; }
    // Headless only: write the next rendered frame to a PNG (tests / CI).
    // Env: VE_CAPTURE=out.png [VE_CAPTURE_FRAME=N, default 150].
    void requestCapture(const std::string& pngPath) { m_capture.requestCapture(pngPath); }

    // --- Photo capture -------------------------------------------------------------
    // Renders the next frames offscreen at width x height (0 = the window's
    // size), moving the camera by a sub-pixel amount each frame and averaging
    // `samples` frames (in linear colour, after `warmupFrames` that let temporal
    // effects settle), then writes a PNG. Anti-aliases edges and converges the
    // noisy effects (fog, clouds, SSGI). The UI (RenderStage::Overlay) is left
    // out. Each frame waits for the GPU and the window isn't updated meanwhile.
    using PhotoCaptureSettings = ve::PhotoCaptureSettings; // see FrameCapture.hpp
    void beginPhotoCapture(const PhotoCaptureSettings& settings) { m_capture.beginPhoto(settings); }
    bool photoCaptureActive() const { return m_capture.photoActive(); }
    float photoCaptureProgress() const { return m_capture.photoProgress(); }
    const std::string& lastPhotoPath() const { return m_capture.lastPhotoPath(); } // empty until one was written
    // The size the scene is rendered at (the window, or a photo being captured).
    VkExtent2D renderExtent() const;

    // Built-in graph textures.
    TextureId sceneColor() const { return m_sceneColor; }
    TextureId sceneDepth() const { return m_sceneDepth; }
    TextureId sceneNormal() const { return m_sceneNormal; }
    TextureId dirShadowMap() const { return m_dirShadowMap; }
    TextureId pointShadowMap() const { return m_pointShadowMap; }
    TextureId backbuffer() const { return m_backbuffer; }

private:
    struct FrameSlot {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkFence inFlight = VK_NULL_HANDLE;
        Buffer globals;   // FrameGlobals (UBO)
        Buffer lights;    // LightBuffer (SSBO)
        Buffer materials; // GpuMaterial[kMaxMaterials] (SSBO)
        Buffer decals;    // DecalBuffer (SSBO)
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkQueryPool timestamps = VK_NULL_HANDLE;   // GPU pass timing
        std::vector<RenderPass*> timedPasses;      // which pass each query pair belongs to
        bool timestampsWritten = false;
    };
    static constexpr uint32_t kMaxTimestamps = 128;
    float m_timestampPeriodNs = 0.0f;
    std::vector<uint64_t> m_timestampScratch; // readback of one slot's queries (reused)

    void createGlobalLayout();
    void createFrameSlots();
    void createShadowMaps();
    void writeShadowDescriptors();
    friend class InstanceSystem; // rewrites its descriptors when the visible list grows
    void writeInstanceDescriptors();
    void createDefaults();
    void createDefaultPasses();
    bool recreateSwapchain();

    // The steps of render(), in order.
    bool acquireFrame(FrameSlot& slot, uint32_t slotIndex, bool offscreen, uint32_t& imageIndex);
    void readGpuTimings(FrameSlot& slot);
    void prepareFrame(Scene& scene, const Time& time, FrameSlot& slot, uint32_t slotIndex, bool photo);
    void recordFrame(FrameSlot& slot, uint32_t slotIndex, uint32_t imageIndex, bool photo, bool offscreen);
    void submitFrame(FrameSlot& slot, uint32_t imageIndex, bool offscreen);

    void updateJitter(bool photo);
    void gatherScene(Scene& scene);
    void fillGlobals(const Time& time);
    void uploadFrameData(FrameSlot& slot, uint32_t slotIndex);
    VkPipeline buildFullscreenPipeline(const FullscreenPipelineDesc& desc);
    VkPipeline buildComputePipeline(const std::string& shaderName);

    struct RawPipeline {
        VkPipeline pipeline = VK_NULL_HANDLE;
        bool compute = false;
        FullscreenPipelineDesc fullscreen;
        std::string computeShader;
    };
    PipelineHandle addRawPipeline(RawPipeline raw); // reuses a free handle id
    std::vector<RawPipeline> m_rawPipelines; // PipelineHandle id - 1
    std::vector<uint32_t> m_freeRawPipelines;
    std::vector<std::weak_ptr<Shader>> m_shaders; // for hot-reload
    std::unique_ptr<ShaderHotReload> m_hotReload;
    void finishPipelineJobs(bool wait); // (start of each frame: hands finished pipelines to their shaders)
    std::vector<std::shared_ptr<PipelineJob>> m_pipelineJobs;
    Event<const CompileEvent&> m_compileEvents;
    uint64_t m_pipelinesCompiled = 0;

    Window* m_window = nullptr;
    RendererSettings m_settings;
    Environment m_environment;
    std::array<glm::vec4, kGlobalParamCount> m_globalParams{};
    std::vector<std::filesystem::path> m_shaderDirs;

    Device m_device;
    Swapchain m_swapchain;
    Samplers m_samplers;
    Bindless m_bindless;
    bool m_initialized = false;
    bool m_swapchainDirty = false;

    VkDescriptorSetLayout m_globalSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_globalPool = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    std::array<FrameSlot, Device::kFramesInFlight> m_slots{};
    uint64_t m_frameNumber = 0;

    std::unique_ptr<RenderGraph> m_graph;
    TextureId m_sceneColor = kNoTexture;
    TextureId m_sceneDepth = kNoTexture;
    TextureId m_sceneNormal = kNoTexture;
    Image m_headlessTarget; // headless: stands in for the swapchain image
    glm::vec2 m_jitter{0.0f}; // sub-pixel camera offset this frame (pixels)
    bool m_loadingFrames = false;
    glm::vec4 m_loadingClear{0.0f, 0.0f, 0.0f, 1.0f};
    TextureId m_dirShadowMap = kNoTexture;
    TextureId m_pointShadowMap = kNoTexture;
    TextureId m_backbuffer = kNoTexture;

    std::vector<GpuMaterial> m_materials; // CPU copy; changed slots are uploaded per frame
    std::vector<uint32_t> m_freeMaterials;
    uint32_t m_materialHighWater = 0;
    MaterialDirtySet m_materialDirty;             // changed slots not yet in every frame slot's buffer

    std::unique_ptr<SkinningSystem> m_skinning;
    std::unique_ptr<LightBuffer> m_lights; // ~110 KB, heap allocated (only the used part is uploaded)
    std::unique_ptr<DecalBuffer> m_decalBuffer; // ~210 KB, likewise
    DecalSystem m_decals;
    // The per-frame parts (see the header comment).
    SceneGatherer m_sceneGatherer;
    StaticBatcher m_staticBatcher;
    ShadowSetup m_shadowSetup;
    LightClusterer m_lightClusterer;
    FrameCapture m_capture;
    TemporalAAPass* m_taaPass = nullptr;  // the camera only jitters while it runs
    FrameData m_frame;
    glm::mat4 m_prevViewProj{1.0f};
    float m_gpuFrameMs = 0.0f;
    float m_lastGpuFrameMs = 0.0f;
    float m_lastFenceWaitMs = 0.0f;
    float m_lastSubmitMs = 0.0f;
    uint64_t m_contentEpoch = 1;

    std::array<std::shared_ptr<Shader>, size_t(BuiltinShader::Count)> m_builtinShaders;
    std::shared_ptr<Texture> m_white, m_black, m_flatNormal;
    std::shared_ptr<StandardMaterial> m_fallbackMaterial; // used when a MeshRenderer has no material
    std::unique_ptr<InstanceSystem> m_instances;
    std::vector<ShadowCaster*> m_shadowCasters;
};

} // namespace ve
