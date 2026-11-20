#pragma once

// =============================================================================
// GPU-driven instancing.
//
// An InstanceGroup draws many copies of one mesh (with optional LODs) with
// per-instance transforms, without GameObjects and without per-object CPU
// work. Every frame a compute shader culls each instance against the camera
// and every directional shadow cascade, picks its LOD, and writes indirect
// draw commands; each pass then issues one draw per (LOD, submesh) batch.
//
//   ve::InstanceGroupDesc desc;
//   desc.lods = {{acaciaMesh, {barkAndLeaves}, 120.0f},     // LOD 0 up to 120 m
//                {acaciaLowMesh, {barkAndLeaves}}};          // LOD 1 beyond
//   desc.cullDistance = 1500.0f;
//   auto trees = renderer.createInstanceGroup(desc, /*capacity*/ 20000);
//   auto h = trees->add(transformMatrix, glm::vec4(tint, 1.0f));   // w > 0: tint vertex colour
//   trees->remove(h);                                              // e.g. tree chopped down
//
// Materials work unchanged as long as their vertex shader uses
// veModelMatrix() (all engine shaders do). Transparent materials are not
// supported in instance groups (they'd need per-instance sorting).
// =============================================================================

#include "ve/gfx/Resources.hpp"
#include "ve/render/GpuTypes.hpp"
#include "ve/render/Material.hpp"
#include "ve/render/PipelineHandle.hpp"

#include <cfloat>
#include <memory>
#include <string>
#include <vector>

namespace ve {

class Renderer;
class Mesh;
class RenderContext;
struct FrameData;
class InstanceSystem;

// One level of detail. Selection is by screen size if the group's first LOD
// sets screenSize > 0, otherwise by distance.
struct InstanceLod {
    std::shared_ptr<Mesh> mesh;
    std::vector<std::shared_ptr<Material>> materials; // per submesh material slot (like MeshRenderer)
    // Screen-size mode: this LOD is used while the instance's bounding radius
    // covers at least this fraction of half the screen height (0.1 ~ 10%).
    // The last LOD's value is ignored.
    float screenSize = 0.0f;
    // Distance mode: camera distance up to which this LOD is used.
    float maxDistance = FLT_MAX;
};

struct InstanceGroupDesc {
    std::string name = "instances";
    std::vector<InstanceLod> lods; // 1..4, nearest first
    bool castShadows = true;       // directional cascades (point-light shadows skip instances)
    float cullDistance = FLT_MAX;  // not drawn beyond this camera distance
    float shadowDistance = FLT_MAX; // no shadow casting beyond this camera distance (small props: keep it short)
};

class InstanceGroup {
public:
    using Handle = uint32_t;
    static constexpr Handle kInvalid = 0xFFFFFFFFu;

    ~InstanceGroup();
    InstanceGroup(const InstanceGroup&) = delete;
    InstanceGroup& operator=(const InstanceGroup&) = delete;

    // Adds an instance; throws if the group is full (see capacity()).
    Handle add(const glm::mat4& transform, const glm::vec4& custom = glm::vec4(0.0f));
    void set(Handle h, const glm::mat4& transform);
    void setCustom(Handle h, const glm::vec4& custom);
    void remove(Handle h);
    void clear();
    glm::mat4 transform(Handle h) const;

    uint32_t count() const { return static_cast<uint32_t>(m_data.size()); }
    uint32_t capacity() const { return m_capacity; }
    const InstanceGroupDesc& desc() const { return m_desc; }

private:
    friend class InstanceSystem;
    InstanceGroup(InstanceSystem& system, InstanceGroupDesc desc, uint32_t capacity, uint32_t firstInstance);
    void markDirty(uint32_t slot);

    InstanceSystem* m_system;
    InstanceGroupDesc m_desc;
    uint32_t m_capacity;
    uint32_t m_firstInstance; // offset in the global instance buffer
    std::vector<GpuInstance> m_data;          // CPU mirror, [0, count)
    std::vector<uint32_t> m_handleToSlot;     // handle -> slot (kInvalid if free)
    std::vector<Handle> m_slotToHandle;
    std::vector<Handle> m_freeHandles;
    uint32_t m_dirtyBegin = 0, m_dirtyEnd = 0; // slots to upload
    glm::vec4 m_localSphere{0.0f};
};

struct InstanceStats {
    uint32_t groups = 0;
    uint32_t instances = 0;
    uint32_t batches = 0;
    uint32_t visibleCamera = 0; // instances drawn for the camera (all batches), from the GPU, 1-2 frames old
    uint32_t visibleShadow = 0; // instance draws in shadow cascades
    uint32_t visibleLod[4] = {}; // camera instance draws per LOD level (crossfading instances count twice)
    uint32_t drawCalls = 0;     // indirect draws issued last frame
};

// Engine-internal manager (owned by the Renderer).
class InstanceSystem {
public:
    // Cull views: the camera + every possible directional shadow cascade, so
    // shadowed lights added at runtime (applySettings) still get instances.
    static constexpr uint32_t kMaxViews = 1 + kMaxDirectionalLights * kMaxCascades;

    void init(Renderer& renderer, uint32_t maxInstances, uint32_t maxVisiblePerView);
    void shutdown();

    std::shared_ptr<InstanceGroup> createGroup(InstanceGroupDesc desc, uint32_t capacity);

    // Per frame (Renderer): build GPU data for this frame slot, assign cull views.
    void prepare(uint32_t slot, FrameData& frame);
    // In InstanceCullPass: upload changes, reset commands, cull.
    void recordCulling(VkCommandBuffer cmd, uint32_t slot);
    // Occlusion: last frame's depth pyramid (HiZPass) for this frame's camera
    // cull; `levels` = bindless indices, empty = no occlusion culling.
    void setOcclusion(const std::array<uint32_t, kHiZLevels>* levels) { m_hiz = levels ? *levels : std::array<uint32_t, kHiZLevels>{}; m_hizValid = levels != nullptr; }
    // In draw passes: one indirect draw per batch for `cullView`.
    void draw(RenderContext& ctx, uint32_t cullView, PassKind kind, uint32_t shadowViewIndex);
    // After the frame: copy draw counts for stats; read them back when the slot comes around again.
    void recordReadback(VkCommandBuffer cmd, uint32_t slot);
    void collectStats(uint32_t slot);

    const InstanceStats& stats() const { return m_stats; }
    bool empty() const { return m_groups.empty(); }

    // Descriptor bindings for the global set.
    VkBuffer instanceBuffer() const { return m_instances.handle; }
    VkBuffer visibleBuffer() const { return m_visible.handle; }
    VkBuffer commandBuffer() const { return m_commands.handle; }
    VkBuffer groupBuffer(uint32_t slot) const { return m_slots[slot].groups.handle; }
    VkBuffer viewBuffer(uint32_t slot) const { return m_slots[slot].views.handle; }

    // InstanceGroup callbacks.
    void release(InstanceGroup* group);

private:
    struct Batch {
        InstanceGroup* group;
        uint32_t lod;
        const Mesh* mesh;
        uint32_t firstIndex, indexCount;
        Material* material;
        uint32_t visibleOffset; // within a view's region of the visible buffer
    };
    struct Slot {
        Buffer groups;   // GpuInstanceGroup[]
        Buffer views;    // GpuCullView[]
        Buffer commands; // DrawIndexedIndirect template (instanceCount = 0)
        Buffer staging;  // instance uploads
        Buffer readback; // command copies for stats
        struct Copy {
            VkDeviceSize src, dst, size;
        };
        std::vector<Copy> uploads;
        uint32_t batchCount = 0, viewCount = 0;
        std::vector<uint8_t> batchLods; // LOD level of each batch (stats)
        bool hasReadback = false;
    };
    struct Range {
        uint32_t first, count;
    };

    void rebuildLayout();
    void ensureVisibleCapacity(uint32_t entries);
    uint32_t allocateRange(uint32_t count);

    Renderer* m_renderer = nullptr;
    uint32_t m_maxInstances = 0, m_maxVisiblePerView = 0;
    static constexpr uint32_t kMaxGroups = 1024;
    static constexpr uint32_t kMaxBatches = 2048;
    Buffer m_instances; // GpuInstance[maxInstances], device local
    Buffer m_visible;   // uint[m_visibleCapacity]: active views x m_visiblePerView, grown on demand
    Buffer m_commands;  // DrawIndexedIndirect[kMaxViews * kMaxBatches]
    uint32_t m_visibleCapacity = 0;
    Slot m_slots[2];
    std::vector<InstanceGroup*> m_groups;
    std::vector<Batch> m_batches;
    // Batches each kind of pass draws (opaque/cutout for the camera, casters
    // for shadows), sorted by pipeline then mesh to minimise binds.
    std::vector<uint32_t> m_forwardBatches, m_shadowBatches;
    std::vector<Range> m_freeRanges;
    uint32_t m_visiblePerView = 0;
    uint32_t m_activeViews = 1;
    uint32_t m_drawsThisFrame = 0;
    std::vector<VkBufferCopy> m_copyRegions; // recordCulling scratch (reused every frame)
    float m_lodScale = 1.0f;
    std::array<uint32_t, kHiZLevels> m_hiz{};
    bool m_hizValid = false;
    bool m_layoutDirty = true;
    PipelineHandle m_cullPipeline;
    InstanceStats m_stats;
};

} // namespace ve
