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

/// One shader or pipeline compile starting or ending (Renderer::compileEvents()).
struct CompileEvent {
    /// Started, finished, or failed (see `error`).
    enum class Type { Started, Finished, Failed };
    /// A pipeline (driver compile of a shader for one pass kind) or a shader (GLSL recompiled by hot-reload).
    enum class What { Pipeline, Shader };
    Type type = Type::Started;  ///< See Type.
    What what = What::Pipeline;  ///< See What.
    std::string name;          ///< "standard.frag (Forward)", "bloom_up.frag"
    float milliseconds = 0.0f; ///< Finished / Failed: how long it took
    uint32_t pending = 0;      ///< compiles of this kind still running after this event
    std::string error;         ///< Failed
};

/// Compiles in progress (Renderer::compileStatus()), e.g. for a "Compiling shaders" note.
struct CompileStatus {
    uint32_t pipelinesPending = 0;  ///< Pipelines being built in the background.
    uint32_t shadersPending = 0;    ///< hot-reload GLSL compiles
    uint64_t pipelinesCompiled = 0; ///< since start
    /// Whether anything is compiling.
    bool busy() const { return pipelinesPending + shadersPending > 0; }
};


class Renderer;
class Window;
class Scene;
class Texture;
class TemporalAAPass;
class Mesh;
struct Time;


/// Hemispherical ambient light. Effects (e.g. vfx::AtmospherePass) usually
/// drive this each frame from the sky.
struct Environment {
    glm::vec3 ambientSky{0.35f, 0.42f, 0.55f};  ///< Light from above (linear RGB).
    glm::vec3 ambientGround{0.18f, 0.15f, 0.12f};  ///< Light from below (linear RGB).
    float ambientIntensity = 1.0f;  ///< Multiplies both.
    /// Fraction of ambientGround that is sunlight bounced off the ground. Shaders
    /// scale that part by sun visibility (veAmbientGround), so objects don't get
    /// a warm "sunlit ground" glow once the sun is behind terrain.
    float groundSunFraction = 0.0f;
};

/// The renderer's own shaders (Renderer::builtinShader).
enum class BuiltinShader { Standard, StandardCutout, StandardTransparent, Unlit, Impostor, Count };

/// Something that draws its own geometry into the directional shadow cascades
/// (e.g. GrassPass). ShadowPass calls drawShadow() for every directional
/// cascade while that cascade's depth target is bound, after the scene's
/// meshes and instances. Register with Renderer::addShadowCaster.
class ShadowCaster {
public:
    virtual ~ShadowCaster() = default;
    /// Draws this caster's geometry into one cascade (its depth target is bound).
    virtual void drawShadow(RenderContext& ctx, const ShadowView& view) = 0;
};

/// The Vulkan renderer: device, swapchain, render graph, materials and
/// shaders, instancing, shadows, decals, frame capture. Engine::renderer();
/// main thread only. Rendering itself is RenderPasses on graph().
class Renderer {
public:
    Renderer() = default;
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    /// Engine: creates the device, swapchain, built-in shaders and passes.
    void init(Window& window, const RendererSettings& settings, bool validation,
              std::vector<std::filesystem::path> shaderDirectories);
    /// Engine: waits for the GPU and destroys everything.
    void shutdown();

    /// Renders one frame of `scene` (returns false if nothing was drawn, e.g. minimized).
    bool render(Scene& scene, const Time& time);
    /// The window was resized: the swapchain is recreated next frame.
    void onResize() { m_swapchainDirty = true; }

    // --- Access ---------------------------------------------------------------
    /// The Vulkan device (queues, memory, deferred destruction).
    Device& device() { return m_device; }
    /// The bindless descriptor tables (textures, buffers by index).
    Bindless& bindless() { return m_bindless; }
    /// The shared samplers.
    const Samplers& samplers() const { return m_samplers; }
    /// The render graph: add or find passes, create textures.
    RenderGraph& graph() { return *m_graph; }
    /// The swapchain (images, format, extent).
    const Swapchain& swapchain() const { return m_swapchain; }
    /// The pipeline layout every pipeline uses (bindless sets + push constants).
    VkPipelineLayout pipelineLayout() const { return m_pipelineLayout; }
    /// This frame's gathered data (camera, lights, draw items).
    const FrameData& frameData() const { return m_frame; }
    /// Draw counts of the last frame.
    const RenderStats& stats() const { return m_frame.stats; }
    /// GPU time per frame in ms (smoothed).
    float gpuFrameTimeMs() const { return m_gpuFrameMs; }
    float lastGpuFrameTimeMs() const { return m_lastGpuFrameMs; } ///< raw, latest read back
    float lastFenceWaitMs() const { return m_lastFenceWaitMs; }  ///< CPU time spent waiting for the GPU in render()
    float lastSubmitMs() const { return m_lastSubmitMs; }        ///< CPU time inside queue submit + present (driver)
    /// Bumped whenever cached GPU content may be stale (shadow maps recreated,
    /// shaders hot-reloaded). Passes that keep results across frames compare it.
    uint64_t contentEpoch() const { return m_contentEpoch; }
    /// The window it presents to.
    Window& window() { return *m_window; }

    /// The renderer's options (change, then applySettings() for shadows / vsync).
    RendererSettings& settings() { return m_settings; }
    /// Call after changing shadow counts/resolutions/vsync in settings().
    void applySettings();

    /// The ambient light (effects usually drive it from the sky).
    Environment& environment() { return m_environment; }
    /// Free-form vec4 globals visible to every shader as frame.globalParams[i].
    void setGlobalParam(uint32_t index, const glm::vec4& value);
    /// A value set with setGlobalParam.
    glm::vec4 globalParam(uint32_t index) const;

    // --- Formats used by the built-in targets ----------------------------------
    /// Format of the HDR scene colour target.
    static constexpr VkFormat kSceneColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    /// Thin G-buffer written by opaque geometry: xyz = world normal, w = roughness
    /// (all zero where nothing was drawn). Used by screen-space effects (SSR, SSGI).
    static constexpr VkFormat kSceneNormalFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    /// Format of the scene depth (reversed Z: 1 = near).
    static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
    /// Format of the shadow maps.
    static constexpr VkFormat kShadowFormat = VK_FORMAT_D32_SFLOAT;

    // --- Resources ----------------------------------------------------------------
    /// A shader (pipeline state + GLSL programs) to make materials with; its pipelines start compiling now (async).
    std::shared_ptr<Shader> createShader(const ShaderDesc& desc);
    /// One of the renderer's built-in shaders.
    std::shared_ptr<Shader> builtinShader(BuiltinShader which) const { return m_builtinShaders[size_t(which)]; }
    /// A material using `shader` (its parameters in the global material buffer).
    std::shared_ptr<Material> createMaterial(std::shared_ptr<Shader> shader);
    /// GPU skinning of SkinnedMeshRenderers.
    SkinningSystem& skinning() { return *m_skinning; }
    /// Loading screens (Engine::beginLoading): frames draw only the UI over a
    /// cleared screen, plus the passes with runsWhileLoading.
    void setLoadingFrames(bool on, const glm::vec4& clearColor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f))
    {
        m_loadingFrames = on;
        m_loadingClear = clearColor;
    }
    /// Whether loading-screen frames are on.
    bool loadingFrames() const { return m_loadingFrames; }
    /// The loading screen's clear colour.
    const glm::vec4& loadingClearColor() const { return m_loadingClear; }
    /// Thousands of copies of a mesh, culled and drawn on the GPU (see Instancing.hpp).
    std::shared_ptr<InstanceGroup> createInstanceGroup(InstanceGroupDesc desc, uint32_t capacity);
    /// The GPU instancing system (instance groups, culling).
    InstanceSystem& instances() { return *m_instances; }
    /// Static MeshRenderers batched into instance groups (RendererSettings::staticBatching).
    StaticBatcher& staticBatcher() { return m_staticBatcher; }
    /// Fire-and-forget decals (bullet holes, footprints): see Decals.hpp.
    /// Placed decals are DecalProjector components.
    DecalSystem& decals() { return m_decals; }
    /// Registers something that draws into the directional shadow cascades.
    void addShadowCaster(ShadowCaster* caster) { m_shadowCasters.push_back(caster); }
    /// Unregisters a shadow caster.
    void removeShadowCaster(ShadowCaster* caster) { std::erase(m_shadowCasters, caster); }
    /// Every registered shadow caster.
    const std::vector<ShadowCaster*>& shadowCasters() const { return m_shadowCasters; }
    /// The material used when a mesh has none (plain light grey standard material).
    Material* fallbackMaterial() const { return m_fallbackMaterial.get(); }
    /// A material with the standard PBR shader for `mode` (opaque, cutout, transparent).
    std::shared_ptr<StandardMaterial> createStandardMaterial(StandardMaterial::Mode mode = StandardMaterial::Mode::Opaque);
    /// A 1x1 white texture (multiplies to no change).
    const std::shared_ptr<Texture>& whiteTexture() const { return m_white; }
    /// A 1x1 black texture.
    const std::shared_ptr<Texture>& blackTexture() const { return m_black; }
    /// A 1x1 flat normal map.
    const std::shared_ptr<Texture>& flatNormalTexture() const { return m_flatNormal; }

    /// Finds `<name>.spv` in the shader directories and creates a module.
    /// The caller destroys it (vkDestroyShaderModule) after building pipelines.
    VkShaderModule loadShader(const std::string& name);
    /// The `.spv` file for a shader name in the shader directories; throws if missing.
    std::filesystem::path findShader(const std::string& name) const;

    /// Builds a pipeline that draws a full-screen triangle with `fragment`
    /// (uses the built-in fullscreen.vert). Optional depth test against a
    /// read-only depth attachment (e.g. sky drawn only where depth == far).
    struct FullscreenPipelineDesc {
        std::string fragment;  ///< Fragment shader name ("bloom_down.frag").
        std::vector<VkFormat> colorFormats;  ///< Formats of the colour attachments it writes.
        BlendMode blend = BlendMode::Opaque;  ///< Blending of the colour attachments.
        VkFormat depthFormat = VK_FORMAT_UNDEFINED;  ///< Depth attachment format (UNDEFINED = none).
        VkCompareOp depthCompare = VK_COMPARE_OP_ALWAYS;  ///< Depth test against that attachment (read only).
    };
    /// A full-screen pipeline (see FullscreenPipelineDesc); rebuilt on shader hot-reload.
    PipelineHandle createFullscreenPipeline(const FullscreenPipelineDesc& desc);
    /// Compute pipeline using the global layout.
    PipelineHandle createComputePipeline(const std::string& shaderName);
    /// Destroys a pipeline once the GPU is done with it.
    void destroyPipeline(VkPipeline pipeline);
    void destroyPipeline(PipelineHandle& pipeline); ///< also resets the handle
    /// The current VkPipeline behind a handle (null if destroyed).
    VkPipeline pipeline(const PipelineHandle& h) const;

    /// Rebuilds every pipeline that uses one of these shaders (e.g.
    /// "bloom_up.frag"); called by the shader hot-reload watcher. Pipelines
    /// whose rebuild fails keep the old version.
    void reloadShaders(const std::vector<std::string>& names);
    /// The shader hot-reload watcher (null when off).
    ShaderHotReload* shaderHotReload() { return m_hotReload.get(); }

    /// Draws `items[indices]` with each material's pipeline for `kind`.
    /// Used by the built-in passes; handy for custom geometry passes too.
    void drawItems(RenderContext& ctx, const std::vector<uint32_t>& indices, PassKind kind,
                   uint32_t viewIndex = 0, uint32_t lightIndex = 0, uint32_t flags = 0);

    // --- Engine internal ------------------------------------------------------------
    /// Builds a material shader's pipeline for one pass kind (thread-safe; requestPipeline schedules it).
    VkPipeline buildMeshPipeline(const ShaderDesc& desc, PassKind kind);

    // ---- Compiles (for "compiling shaders..." UI) ----
    /// Pipelines: a shader's first use per pass kind, built by the driver
    /// (async: RendererSettings::asyncPipelines). Shaders: GLSL recompiled by
    /// shader hot-reload (development builds).
    CompileStatus compileStatus() const;
    /// Started / Finished / Failed per compile, on the main thread (async ones
    /// finish at the start of a later frame).
    Event<const CompileEvent&>& compileEvents() { return m_compileEvents; }
    /// Blocks until every pipeline compile in flight is done (screenshots, tests).
    void waitForPipelines();
    /// (Shader's side: starts building one of its pipelines; drops its jobs.)
    void requestPipeline(Shader& shader, PassKind kind);
    /// Drops a shader's in-flight pipeline jobs (their results are destroyed when done).
    void detachPipelineJobs(Shader& shader);
    /// Material: reserves a slot in the global material buffer.
    uint32_t allocateMaterialSlot();
    /// Material: frees its slot.
    void freeMaterialSlot(uint32_t index);
    /// A material slot's GPU data (writing it doesn't mark it for upload: use Material::data()).
    GpuMaterial& materialData(uint32_t index) { return m_materials[index]; }
    /// Marks a material slot for upload.
    void markMaterialDirty(uint32_t index) { m_materialDirty.mark(index); }
    /// Binds the global descriptor sets (frame data, bindless) for a command buffer.
    void bindGlobalSets(VkCommandBuffer cmd);
    /// Copies all material parameters to the GPU now (normally done per frame);
    /// for GPU work at load time that reads materials, e.g. impostor baking.
    void uploadMaterialsNow();
    /// Frames rendered so far.
    uint64_t frameNumber() const { return m_frameNumber; }
    /// Headless only: write the next rendered frame to a PNG (tests / CI).
    /// Env: VE_CAPTURE=out.png [VE_CAPTURE_FRAME=N, default 150].
    void requestCapture(const std::string& pngPath) { m_capture.requestCapture(pngPath); }

    // --- Photo capture -------------------------------------------------------------
    /// Renders the next frames offscreen at width x height (0 = the window's
    /// size), moving the camera by a sub-pixel amount each frame and averaging
    /// `samples` frames (in linear colour, after `warmupFrames` that let temporal
    /// effects settle), then writes a PNG. Anti-aliases edges and converges the
    /// noisy effects (fog, clouds, SSGI). The UI (RenderStage::Overlay) is left
    /// out. Each frame waits for the GPU and the window isn't updated meanwhile.
    using PhotoCaptureSettings = ve::PhotoCaptureSettings; ///< see FrameCapture.hpp
    /// Starts a photo capture (see above).
    void beginPhotoCapture(const PhotoCaptureSettings& settings) { m_capture.beginPhoto(settings); }
    /// Whether a photo capture is running.
    bool photoCaptureActive() const { return m_capture.photoActive(); }
    /// Progress of the photo capture (0..1).
    float photoCaptureProgress() const { return m_capture.photoProgress(); }
    const std::string& lastPhotoPath() const { return m_capture.lastPhotoPath(); } ///< empty until one was written
    /// The size the scene is rendered at (the window, or a photo being captured).
    VkExtent2D renderExtent() const;

    /// Built-in graph textures.
    TextureId sceneColor() const { return m_sceneColor; }
    /// The scene depth target (reversed Z).
    TextureId sceneDepth() const { return m_sceneDepth; }
    /// The thin G-buffer (world normal + roughness).
    TextureId sceneNormal() const { return m_sceneNormal; }
    /// The directional shadow map array (one layer per cascade).
    TextureId dirShadowMap() const { return m_dirShadowMap; }
    /// The point-light shadow cube array.
    TextureId pointShadowMap() const { return m_pointShadowMap; }
    /// The swapchain image being drawn this frame.
    TextureId backbuffer() const { return m_backbuffer; }

private:
    struct FrameSlot {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkFence inFlight = VK_NULL_HANDLE;
        Buffer globals;   ///< FrameGlobals (UBO)
        Buffer lights;    ///< LightBuffer (SSBO)
        Buffer materials; ///< GpuMaterial[kMaxMaterials] (SSBO)
        Buffer decals;    ///< DecalBuffer (SSBO)
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkQueryPool timestamps = VK_NULL_HANDLE;   ///< GPU pass timing
        std::vector<RenderPass*> timedPasses;      ///< which pass each query pair belongs to
        bool timestampsWritten = false;
    };
    static constexpr uint32_t kMaxTimestamps = 128;
    float m_timestampPeriodNs = 0.0f;
    std::vector<uint64_t> m_timestampScratch; ///< readback of one slot's queries (reused)

    void createGlobalLayout();
    void createFrameSlots();
    void createShadowMaps();
    void writeShadowDescriptors();
    friend class InstanceSystem; // rewrites its descriptors when the visible list grows
    void writeInstanceDescriptors();
    void createDefaults();
    void createDefaultPasses();
    bool recreateSwapchain();

    /// The steps of render(), in order.
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
    PipelineHandle addRawPipeline(RawPipeline raw); ///< reuses a free handle id
    std::vector<RawPipeline> m_rawPipelines; ///< PipelineHandle id - 1
    std::vector<uint32_t> m_freeRawPipelines;
    std::vector<std::weak_ptr<Shader>> m_shaders; ///< for hot-reload
    std::unique_ptr<ShaderHotReload> m_hotReload;
    void finishPipelineJobs(bool wait); ///< (start of each frame: hands finished pipelines to their shaders)
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
    Image m_headlessTarget; ///< headless: stands in for the swapchain image
    glm::vec2 m_jitter{0.0f}; ///< sub-pixel camera offset this frame (pixels)
    bool m_loadingFrames = false;
    glm::vec4 m_loadingClear{0.0f, 0.0f, 0.0f, 1.0f};
    TextureId m_dirShadowMap = kNoTexture;
    TextureId m_pointShadowMap = kNoTexture;
    TextureId m_backbuffer = kNoTexture;

    std::vector<GpuMaterial> m_materials; ///< CPU copy; changed slots are uploaded per frame
    std::vector<uint32_t> m_freeMaterials;
    uint32_t m_materialHighWater = 0;
    MaterialDirtySet m_materialDirty;             ///< changed slots not yet in every frame slot's buffer

    std::unique_ptr<SkinningSystem> m_skinning;
    std::unique_ptr<LightBuffer> m_lights; ///< ~110 KB, heap allocated (only the used part is uploaded)
    std::unique_ptr<DecalBuffer> m_decalBuffer; ///< ~210 KB, likewise
    DecalSystem m_decals;
    /// The per-frame parts (see the header comment).
    SceneGatherer m_sceneGatherer;
    StaticBatcher m_staticBatcher;
    ShadowSetup m_shadowSetup;
    LightClusterer m_lightClusterer;
    FrameCapture m_capture;
    TemporalAAPass* m_taaPass = nullptr;  ///< the camera only jitters while it runs
    FrameData m_frame;
    glm::mat4 m_prevViewProj{1.0f};
    float m_gpuFrameMs = 0.0f;
    float m_lastGpuFrameMs = 0.0f;
    float m_lastFenceWaitMs = 0.0f;
    float m_lastSubmitMs = 0.0f;
    uint64_t m_contentEpoch = 1;

    std::array<std::shared_ptr<Shader>, size_t(BuiltinShader::Count)> m_builtinShaders;
    std::shared_ptr<Texture> m_white, m_black, m_flatNormal;
    std::shared_ptr<StandardMaterial> m_fallbackMaterial; ///< used when a MeshRenderer has no material
    std::unique_ptr<InstanceSystem> m_instances;
    std::vector<ShadowCaster*> m_shadowCasters;
};

} // namespace ve
