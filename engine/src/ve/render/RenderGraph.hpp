#pragma once

// =============================================================================
// RenderGraph - an ordered list of RenderPasses with automatic image barriers.
//
// Why: Vulkan requires explicit layout transitions and memory barriers
// between passes ("the fog pass reads SceneDepth that the opaque pass
// wrote"). Writing those by hand for every combination is error prone. Here
// each pass *declares* how it uses each texture this frame, and the graph
// emits the right vkCmdPipelineBarrier2 before running it.
//
// Usage from a game/effect:
//
//   class MyPostPass : public ve::RenderPass {
//   public:
//       MyPostPass() : RenderPass("MyPost", RenderStage::PostProcess) {}
//       void setup(RenderGraph& g) override { m_temp = g.createTexture("MyTemp", {...}); }
//       void declare(PassBuilder& b) override {
//           b.use(b.graph().texture("SceneColor"), Access::Sampled);
//           b.use(m_temp, Access::ColorAttachment);
//       }
//       void execute(RenderContext& ctx) override { ... }
//   };
//   renderer.graph().addPass<MyPostPass>();
//
// Deliberate simplifications: passes run in (stage, order) order; no pass
// culling, no memory aliasing, single queue.
// =============================================================================

#include "ve/gfx/Resources.hpp"
#include "ve/gfx/Samplers.hpp"
#include "ve/render/FrameData.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ve {

class Renderer;
class RenderGraph;

using TextureId = uint32_t;
constexpr TextureId kNoTexture = 0xFFFFFFFFu;

enum class RenderStage : int {
    Shadow = 100,
    Opaque = 200,
    Sky = 300,
    AfterOpaque = 400,
    Transparent = 500,
    PostProcess = 600,
    Composite = 700,
    Overlay = 800,
};

enum class Access {
    ColorAttachment, // render target (read + write)
    DepthAttachment, // depth test + write
    DepthRead,       // depth test without writes; can be sampled at the same time
    Sampled,         // texture() in fragment/compute shaders
    StorageRead,     // imageLoad in compute/fragment
    StorageWrite,    // imageStore
    StorageReadWrite,
    TransferSrc,
    TransferDst,
    Present,
};

struct GraphTextureDesc {
    VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;
    // Size: if width/height are 0 the texture is screen-relative, scaled by
    // `scale` (0.5 = half resolution) and recreated on resize.
    float scale = 1.0f;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t layers = 1;
    uint32_t mips = 1;
    VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;
    bool perLayerViews = false; // one 2D view per layer (to render into layers)
    bool storage = false;       // allow imageLoad/imageStore (format must support it)
    VkImageUsageFlags extraUsage = 0;
    SamplerType sampler = SamplerType::LinearClamp;
};

struct ResourceState {
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 access = VK_ACCESS_2_NONE;
};

struct GraphTexture {
    std::string name;
    GraphTextureDesc desc;
    Image image;
    VkExtent2D extent{};
    uint32_t sampledIndex = 0xFFFFFFFFu; // bindless texture index (sample in any shader)
    uint32_t storageIndex = 0xFFFFFFFFu; // bindless storage image index (if desc.storage)
    ResourceState state;
    bool imported = false;
};

class PassBuilder {
public:
    PassBuilder(RenderGraph& graph) : m_graph(graph) {}
    void use(TextureId texture, Access access);
    RenderGraph& graph() { return m_graph; }

    struct Usage {
        TextureId texture;
        Access access;
    };
    std::vector<Usage> usages;

private:
    RenderGraph& m_graph;
};

struct ColorTarget {
    TextureId texture = kNoTexture;
    bool clear = false;
    glm::vec4 clearValue{0.0f};
    int layer = -1; // -1 = whole image view, otherwise per-layer view
};

struct DepthTarget {
    TextureId texture = kNoTexture;
    bool clear = false;
    float clearValue = 0.0f;
    int layer = -1;
    bool readOnly = false; // must match Access::DepthRead
};

// Handed to RenderPass::execute(). Owns nothing; just convenience helpers.
class RenderContext {
public:
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    Renderer* renderer = nullptr;
    RenderGraph* graph = nullptr;
    FrameData* frame = nullptr;
    uint32_t frameSlot = 0;

    // Begins dynamic rendering and sets viewport/scissor to the target size.
    void beginRendering(std::initializer_list<ColorTarget> colors, const DepthTarget& depth = {});
    void endRendering();
    void setViewport(VkExtent2D extent);
    void bindPipeline(VkPipeline pipeline, VkPipelineBindPoint point = VK_PIPELINE_BIND_POINT_GRAPHICS);
    void pushConstants(const void* data, uint32_t size);
    template <class T>
    void push(const T& data)
    {
        static_assert(sizeof(T) <= kPushConstantBytes, "push constants are limited to 128 bytes");
        pushConstants(&data, sizeof(T));
    }
    void drawFullscreen(); // 3-vertex triangle, see fullscreen.vert

    // A whole full-screen draw: begin rendering into `colors`, bind, push, draw, end.
    //   ctx.fullscreen({{m_output}}, m_pipeline, MyPush{...});
    template <class T>
    void fullscreen(std::initializer_list<ColorTarget> colors, VkPipeline pipeline, const T& pushData)
    {
        beginRendering(colors);
        bindPipeline(pipeline);
        push(pushData);
        drawFullscreen();
        endRendering();
    }
};

class RenderPass {
public:
    RenderPass(std::string name, RenderStage stage, int order = 0);
    virtual ~RenderPass() = default;

    // Called once when added to the graph: create textures and pipelines here.
    virtual void setup(RenderGraph&) {}
    // CPU work before GPU buffers are uploaded; may modify frame.globals.
    virtual void prepare(FrameData&) {}
    // Declare texture usage for this frame.
    virtual void declare(PassBuilder& builder) = 0;
    // Record commands.
    virtual void execute(RenderContext& ctx) = 0;
    // Screen-relative textures were recreated (bindless indices stay the same).
    virtual void onResize(RenderGraph&) {}
    // Debug UI (inside an ImGui tree node in the F1 overlay).
    virtual void onGui() {}

    const std::string& name() const { return m_name; }
    RenderStage stage() const { return m_stage; }
    int order() const { return m_order; }
    bool enabled = true;
    // Also runs on loading-screen frames (Renderer::setLoadingFrames), which
    // otherwise only draw the UI: work the world's streaming relies on
    // (instance uploads) and the UI itself.
    bool runsWhileLoading = false;
    float gpuTimeMs = 0.0f;     // measured with GPU timestamps, smoothed (shown in the F1 overlay)
    float lastGpuTimeMs = 0.0f; // the latest raw measurement (benchmarks)

protected:
    Renderer& renderer() const { return *m_renderer; }
    RenderGraph& graph() const { return *m_graph; }

private:
    friend class RenderGraph;
    std::string m_name;
    RenderStage m_stage;
    int m_order;
    uint64_t m_sequence = 0;
    Renderer* m_renderer = nullptr;
    RenderGraph* m_graph = nullptr;
};

class RenderGraph {
public:
    explicit RenderGraph(Renderer& renderer);
    ~RenderGraph();

    RenderGraph(const RenderGraph&) = delete;
    RenderGraph& operator=(const RenderGraph&) = delete;

    // --- Textures ----------------------------------------------------------
    TextureId createTexture(const std::string& name, const GraphTextureDesc& desc);
    // Recreate a fixed-size texture with a new description (e.g. shadow settings changed).
    void recreateTexture(TextureId id, const GraphTextureDesc& desc);
    TextureId importTexture(const std::string& name); // external image, set each frame
    void setImported(TextureId id, VkImage image, VkImageView view, VkFormat format, VkExtent2D extent,
                     const ResourceState& state);
    TextureId texture(const std::string& name) const; // kNoTexture if missing
    GraphTexture& get(TextureId id) { return m_textures[id]; }
    const GraphTexture& get(TextureId id) const { return m_textures[id]; }
    // Bindless index to sample `id` with in shaders (veSample(index, uv)).
    uint32_t sampled(TextureId id) const { return m_textures[id].sampledIndex; }
    // Clears an image once and leaves it READ_ONLY (so it can be sampled before being rendered).
    void initializeDepth(TextureId id, float clearValue);

    // --- Passes ------------------------------------------------------------
    template <class T, class... Args>
    T& addPass(Args&&... args)
    {
        auto pass = std::make_unique<T>(std::forward<Args>(args)...);
        T& ref = *pass;
        addPassImpl(std::move(pass));
        return ref;
    }
    void removePass(const std::string& name);
    RenderPass* findPass(const std::string& name) const;
    template <class T>
    T* findPass() const
    {
        for (const auto& p : m_passes) {
            if (auto* t = dynamic_cast<T*>(p.get())) return t;
        }
        return nullptr;
    }
    const std::vector<std::unique_ptr<RenderPass>>& passes() const { return m_passes; }

    // --- Engine internal -----------------------------------------------------
    void resize(VkExtent2D screenExtent);
    VkExtent2D screenExtent() const { return m_screenExtent; }
    void prepare(FrameData& frame);
    void execute(RenderContext& ctx);
    // GPU timing: if a query pool is set, execute() writes a timestamp pair
    // around every pass (2 queries per pass, starting at query 0).
    void setTimestampPool(VkQueryPool pool, uint32_t capacity) { m_queryPool = pool; m_queryCapacity = capacity; }
    // Called by the renderer with the resolved timings of a previous frame.
    void applyTimings(const std::vector<uint64_t>& timestamps, const std::vector<RenderPass*>& order, float periodNs);
    // Passes that got a timestamp pair in the last execute(), in query order
    // (pair i = queries 2i, 2i+1; never more than the pool's capacity).
    const std::vector<RenderPass*>& lastTimed() const { return m_timed; }
    // Passes at this stage or later are skipped (photo capture leaves out the UI).
    int maxStage = 1 << 30;
    // Loading-screen frames: only passes with runsWhileLoading.
    bool loadingFrame = false;
    // Transitions a texture outside of any pass (e.g. backbuffer -> present).
    void transition(VkCommandBuffer cmd, TextureId id, Access access);
    void clearPasses();

private:
    void addPassImpl(std::unique_ptr<RenderPass> pass);
    void createImage(GraphTexture& tex);
    void destroyImage(GraphTexture& tex, bool deferred);
    VkExtent2D resolveExtent(const GraphTextureDesc& desc) const;

    Renderer& m_renderer;
    std::vector<GraphTexture> m_textures;
    std::unordered_map<std::string, TextureId> m_byName;
    std::vector<std::unique_ptr<RenderPass>> m_passes;
    uint64_t m_sequence = 0;
    VkExtent2D m_screenExtent{1, 1};
    std::vector<VkImageMemoryBarrier2> m_barriers; // scratch
    VkQueryPool m_queryPool = VK_NULL_HANDLE;
    uint32_t m_queryCapacity = 0;
    std::vector<RenderPass*> m_timed; // passes timed this frame, in query order
};

} // namespace ve
