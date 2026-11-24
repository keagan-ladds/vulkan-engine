#pragma once

// =============================================================================
// StaticBatcher - static MeshRenderers as GPU instances (part of Renderer).
//
// Things that never move (building pieces, huts, props) cost nothing per
// frame on the CPU once they are here: each one is an instance in an
// InstanceGroup per (mesh, materials, castShadows), so the GPU culls them
// (camera + directional cascades, Hi-Z occlusion) and draws each batch with
// one indirect draw. Groups grow (2x, the instances move over) when full and
// are released when empty.
//
// The batcher is plain render data: SceneGatherer decides which renderers
// are eligible (MeshRenderer::isStatic, active, visible, not skinned, no
// transparent material) and adds, moves and removes entries as the scene
// reports changes (Scene::StaticChanges). Entries keep their world bounds in
// a coarse grid for what instance groups can't do:
//   - point-light shadows: entries within a shadowed light's range become
//     ordinary DrawItems for its cube faces (appendPointShadowCasters), so
//     the face cache (D16) keeps hashing exactly what it draws;
//   - editor picking draws the entries in view (forEachInFrustum).
// =============================================================================

#include "ve/render/FrameData.hpp"
#include "ve/render/Instancing.hpp"
#include "ve/render/Mesh.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace ve {

class GameObject;
class Material;
class Mesh;
class Renderer;

class StaticBatcher {
public:
    using Handle = uint32_t;
    static constexpr Handle kNone = 0xFFFFFFFFu;

    struct Desc {
        std::shared_ptr<Mesh> mesh;
        /// Per submesh material slot (index = Submesh::materialSlot), already
        /// resolved like MeshRenderer::material(); null = the fallback material.
        std::vector<std::shared_ptr<Material>> materials;
        bool castShadows = true;
        glm::mat4 world{1.0f};
        const GameObject* owner = nullptr; ///< for DrawItem::owner (editor picking)
        const void* source = nullptr;      ///< who added it (checked by remove / matches)
    };

    void init(Renderer& renderer) { m_renderer = &renderer; }

    Handle add(const Desc& desc); ///< may throw (instance buffer full: RendererSettings::maxInstances)
    void remove(Handle h);
    void setTransform(Handle h, const glm::mat4& world);
    /// Can `h` keep its entry with these fields (same batch)? Otherwise the
    /// caller removes and adds it again.
    bool matches(Handle h, const void* source, const Mesh* mesh, const std::vector<std::shared_ptr<Material>>& materials,
                 bool castShadows) const;
    void clear(); ///< drops every entry and group
    /// Hidden groups draw nothing (another scene is being rendered).
    void setHidden(bool hidden);
    bool hidden() const { return m_hidden; }

    size_t size() const { return m_entries.size() - m_free.size(); }
    bool empty() const { return size() == 0; }
    uint32_t groupCount() const { return static_cast<uint32_t>(m_groupByKey.size()); }
    uint32_t submeshCount() const { return m_submeshes; } ///< over all entries (RenderStats::totalObjects)
    /// Highest point of any entry (-FLT_MAX if none): ShadowSetup's caster reach.
    float top() const;

    /// Appends a DrawItem (instanced = true) per submesh of every
    /// shadow-casting entry whose bounds reach the sphere, unless this frame
    /// (`stamp`) already appended it for another light.
    void appendPointShadowCasters(const glm::vec3& center, float range, uint64_t stamp, Material* fallback,
                                  std::vector<DrawItem>& out);

    /// fn(mesh, submesh, world, owner) for every submesh of entries intersecting `frustum`.
    template <class F>
    void forEachInFrustum(const Frustum& frustum, F&& fn) const
    {
        for (const Entry& e : m_entries) {
            if (e.group == nullptr || !frustum.intersects(e.bounds)) continue;
            for (const Submesh& sm : e.group->mesh->submeshes()) fn(*e.group->mesh, sm, e.world, e.owner);
        }
    }

private:
    /// Batch key: mesh, resolved materials per slot, castShadows.
    using Key = std::tuple<const Mesh*, std::vector<const Material*>, bool>;
    struct Group {
        Key key;
        std::shared_ptr<Mesh> mesh;
        std::vector<std::shared_ptr<Material>> materials;
        bool castShadows = true;
        std::shared_ptr<InstanceGroup> instances;
        std::vector<Handle> entryOfInstance; ///< InstanceGroup handle -> entry (kNone = free)
        uint32_t count = 0;
    };
    struct Entry {
        Group* group = nullptr; ///< null = free slot
        InstanceGroup::Handle instance = InstanceGroup::kInvalid;
        glm::mat4 world{1.0f};
        AABB bounds; ///< world space, whole mesh
        const GameObject* owner = nullptr;
        const void* source = nullptr;
        uint64_t stamp = 0;      ///< appendPointShadowCasters: last frame it was appended
        bool large = false;      ///< in m_large instead of grid cells
    };

    static Key makeKey(const Mesh* mesh, const std::vector<std::shared_ptr<Material>>& materials, bool castShadows);
    Group& groupFor(const Desc& desc);
    void grow(Group& g);
    void releaseGroup(Group& g);
    InstanceGroupDesc groupDesc(const Group& g) const;

    /// Grid (XZ cells) of entries, for point-light queries.
    static constexpr float kCellSize = 16.0f;
    static constexpr int kMaxCellSpan = 8; ///< larger entries go in m_large
    static uint64_t cellKey(int x, int z) { return (uint64_t(uint32_t(x)) << 32) | uint32_t(z); }
    void insertGrid(Handle h);
    void eraseGrid(Handle h);

    Renderer* m_renderer = nullptr;
    std::vector<Entry> m_entries;
    std::vector<Handle> m_free;
    std::map<Key, std::unique_ptr<Group>> m_groupByKey;
    std::unordered_map<uint64_t, std::vector<Handle>> m_cells;
    std::vector<Handle> m_large;
    uint32_t m_submeshes = 0;
    mutable float m_top = 0.0f;
    mutable bool m_topDirty = true;
    bool m_hidden = false;
};

} // namespace ve
