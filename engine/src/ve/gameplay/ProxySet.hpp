#pragma once

// =============================================================================
// ProxySet - many simple things held as data, drawn instanced, saved as one
// block, and turned into real GameObjects only near the player.
//
//   auto& pickups = scene.spawn<ve::ProxySet>("Pickups");
//   ve::ProxyType stone;
//   stone.name = "stone";
//   stone.lods = {{stoneMesh, {material}}};          // or ve::buildLodChain(...)
//   stone.promoteDistance = 6.0f;                    // a GameObject within 6 m of the focus
//   stone.promote = [](ve::Scene& s, const ve::ProxyEntry& e) {
//       auto& o = s.spawn("Stone");                  // (or set `prefab` instead)
//       o.addComponent<ve::Interactable>().prompt = "Pick up stone";
//       return &o;                                   // the set positions it and adds a ProxyLink
//   };
//   const uint32_t type = pickups.addType(stone);
//   pickups.add(type, position, rotation);           // thousands of these
//   pickups.focus = &player;                         // promotion follows the player (default: main camera)
//
// It is the generic, engine-level form of what FoliageSystem does for
// procedural foliage: each entry (a stable 64-bit id, a type, a transform, a
// uint32 state the game interprets and an optional small JSON payload) is an
// instance of its type's InstanceGroup while it's far away. Within the type's
// promoteDistance of the focus it becomes a GameObject (the type's factory or
// prefab, else a plain object with the far look's first LOD) carrying a
// ProxyLink; its instance is hidden meanwhile. When the focus moves away
// (+ demoteMargin), the object's transform and the state of its Saveable
// object / components are captured back into the entry and the object is
// destroyed. If gameplay destroys the object (picked up), the entry is gone.
// Promotions and demotions are budgeted per frame.
//
// Level content vs runtime: entries added before the set's start() (the scene
// file's `entries`, level-building code) are the level's own and get
// sequential ids (1, 2, ...: the same every run when built the same way);
// entries added later (a dropped item) get random ids. Save games (the set is
// a ve::Saveable) store only the difference to the level's entries: removed
// ids plus every changed or added entry in full. So a level with thousands of
// untouched entries saves almost nothing, and loading a save into a freshly
// built level gives back exactly what was there.
//
// Types are code (meshes, factories): addType() before or after the entries.
// Entries name their type, so a scene file's entries whose type the game
// hasn't added (yet) are kept and saved, just not drawn or promoted.
//
// Limitations: entries are data, not objects: the editor shows a set and its
// entry count, but doesn't paint or place entries yet. Networking: a set is
// local (each peer promotes around its own focus); pickups aren't
// host-authoritative yet (roadmap N2), only a joiner's save snapshot carries
// the host's entries.
// =============================================================================

#include "ve/physics/Physics.hpp"
#include "ve/render/Instancing.hpp"
#include "ve/scene/GameObject.hpp"
#include "ve/scene/ObjectRef.hpp"
#include "ve/scene/Reflection.hpp"
#include "ve/scene/SaveGame.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ve {

class ProxySet;

/// One thing in a set.
struct ProxyEntry {
    uint64_t id = 0;   ///< stable across save/load and level reloads
    uint32_t type = 0; ///< ProxySet::typeIndex
    glm::vec3 position{0.0f};  ///< World position.
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};  ///< World rotation.
    glm::vec3 scale{1.0f};  ///< Scale.
    uint32_t state = 0; ///< the game's: what it means is up to the type's objects
    Json data;          ///< optional small game payload (null = none)
    /// The promoted object's Saveable state, captured when it was demoted and
    /// given back when it's promoted again (the set manages it).
    Json objectState;

    /// The entry's world matrix (position, rotation, scale).
    glm::mat4 matrix() const;
};

/// What a kind of entry looks like far away, and what it becomes up close.
struct ProxyType {
    std::string name = "proxy";  ///< Identifies the type (typeIndex, save games).
    /// The far look: up to 4 LODs (ve::buildLodChain, or an InstanceGroupDesc's
    /// lods). Empty = invisible while it's data.
    std::vector<InstanceLod> lods;
    bool castShadows = true;  ///< The far look casts shadows.
    float cullDistance = 200.0f;  ///< Metres beyond which the far look isn't drawn.
    float shadowDistance = 60.0f;  ///< Metres beyond which it casts no shadow.

    /// > 0: entries this close to the set's focus become GameObjects.
    float promoteDistance = 0.0f;
    float demoteMargin = 0.25f; ///< back to data beyond promoteDistance * (1 + margin)
    /// Creates the promoted object (null = not now). Without it: `prefab`
    /// (instantiated, not recorded by save games: the set saves it), else a
    /// plain GameObject named like the type. Either way the set positions it,
    /// adds a ProxyLink and, unless the object has its own, a MeshRenderer
    /// with the first LOD and a static body with `collider`.
    std::function<GameObject*(Scene&, const ProxyEntry&)> promote;
    std::string prefab;  ///< Prefab asset path instantiated for promotion when `promote` isn't set.
    Collider collider; ///< optional
};

/// On every promoted object: which entry it stands for, and the way to change
/// it (changes outlive the object: the set keeps them).
class ProxyLink : public Component {
public:
    ProxySet* set() const; ///< null if the set is gone
    /// The entry's id in its set.
    uint64_t entry() const { return m_entry; }
    /// The entry this object stands for (null if the set or entry is gone).
    const ProxyEntry* entryData() const;
    /// The entry's game state (0 if gone).
    uint32_t state() const;
    /// Changes the entry's state (kept after the object is demoted).
    void setState(uint32_t state);
    /// Changes the entry's game data (kept after the object is demoted).
    void setData(Json data);
    /// Removes the entry and destroys this object (destroying the object
    /// alone does the same, a frame later).
    void consume();

private:
    friend class ProxySet;
    ObjectRef<ProxySet> m_set;
    uint64_t m_entry = 0;
};

/// A ProxySet's counters.
struct ProxyStats {
    uint32_t entries = 0;  ///< Entries in the set.
    uint32_t promoted = 0;  ///< Entries currently standing as GameObjects.
    uint32_t promotions = 0; ///< since start (counters: tests, the F1 overlay)
    uint32_t demotions = 0;  ///< Since start.
    uint32_t consumed = 0;   ///< entries removed because gameplay destroyed their object
};

/// Many things (thousands of crates, rocks, pickups) kept as data and drawn
/// instanced far away, each becoming a real GameObject when the focus comes
/// close and data again when it leaves (see the top of this file).
class ProxySet : public GameObject, public Saveable {
public:
    /// An empty set; add types, then entries.
    explicit ProxySet(std::string name = "Proxy set");
    ~ProxySet() override;

    // ---- Types ----
    /// Adds a type (or redefines the one with the same name); returns its index.
    uint32_t addType(ProxyType type);
    int typeIndex(const std::string& name) const; ///< -1 = unknown
    const ProxyType* type(uint32_t index) const;  ///< null if only named by entries so far
    /// Number of types.
    uint32_t typeCount() const { return uint32_t(m_types.size()); }

    // ---- Entries ----
    /// id 0 = a new one (sequential before start(): level content; random after).
    uint64_t add(uint32_t type, const glm::vec3& position, const glm::quat& rotation = glm::quat(1, 0, 0, 0),
                 const glm::vec3& scale = glm::vec3(1.0f), uint32_t state = 0, Json data = {}, uint64_t id = 0);
    bool remove(uint64_t id); ///< also destroys its promoted object
    /// The entry with this id, or null.
    const ProxyEntry* find(uint64_t id) const;
    /// Changes an entry's game state; false if there's no such entry.
    bool setState(uint64_t id, uint32_t state);
    /// Changes an entry's game data; false if there's no such entry.
    bool setData(uint64_t id, Json data);
    /// Moves an entry (and its promoted object); false if there's no such entry.
    bool setTransform(uint64_t id, const glm::vec3& position, const glm::quat& rotation, const glm::vec3& scale);
    /// Number of entries.
    size_t size() const { return m_entries.size(); }
    void forEach(const std::function<void(const ProxyEntry&)>& fn) const; ///< unordered
    /// Entries within `radius` of `p` (3D distance), unordered.
    void query(const glm::vec3& p, float radius, const std::function<void(const ProxyEntry&)>& fn) const;
    /// The nearest entry within `radius` (of `type`, or any if -1).
    const ProxyEntry* nearest(const glm::vec3& p, float radius, int type = -1) const;

    // ---- Promotion ----
    /// Promotion follows this object (the player); 0 / gone = the main camera.
    ObjectRef<> focus;
    uint32_t promotionsPerFrame = 8;  ///< At most this many entries become objects per frame (spreads the cost).
    uint32_t demotionsPerFrame = 8;  ///< At most this many objects become data again per frame.
    GameObject* promoted(uint64_t id) const; ///< the object standing in for an entry, else null
    /// Number of entries standing as GameObjects now.
    uint32_t promotedCount() const { return uint32_t(m_promotedIds.size()); }
    /// The set's counters.
    const ProxyStats& stats() const;

    // ---- Persistence ----
    /// Save games: the difference to the level's entries.
    void saveState(Json& out) const override;
    void loadState(const Json& in) override;
    /// Scene files (the hidden `entries` field): every entry, as level content.
    Json entriesJson() const;
    /// Replaces every entry with the ones `json` lists (scene loading).
    void setEntriesJson(const Json& json);

    void start() override;
    void update(float dt) override;
    void onDestroy() override;

private:
    friend class ProxyLink;
    struct Slot {
        ProxyEntry e;
        int group = -1; ///< index into its type's groups, -1 = not drawn
        InstanceGroup::Handle handle = InstanceGroup::kInvalid;
        uint64_t promoted = 0; ///< GameObject id while promoted
    };
    struct TypeData {
        ProxyType desc;
        bool defined = false; ///< false: only named by entries (scene file / save) so far
        std::vector<std::shared_ptr<InstanceGroup>> groups; ///< more are added when they fill up
    };

    uint32_t typeIndexOrAdd(const std::string& name);
    Slot* slot(uint64_t id);
    const Slot* slot(uint64_t id) const;
    void show(Slot& s);
    void hide(Slot& s);
    void promote(Slot& s);
    void demote(Slot& s);
    void erase(uint64_t id, bool destroyObject);
    void gridAdd(const ProxyEntry& e);
    void gridRemove(const ProxyEntry& e);
    void place(Slot& s, const ProxyEntry& from); ///< update an existing slot to `from`
    void upsert(const ProxyEntry& e);
    std::optional<ProxyEntry> current(const Slot& s) const; ///< the entry as it is now (promoted: from its object); null = consumed
    Json entryJson(const ProxyEntry& e, std::unordered_map<uint32_t, uint32_t>& typeMap, Json& types) const;
    std::optional<ProxyEntry> entryFromJson(const Json& j, const std::vector<uint32_t>& typeMap) const;
    bool focusPosition(glm::vec3* out) const;
    static int64_t cellKey(int cx, int cz);
    int cellOf(float v) const;

    std::vector<TypeData> m_types;
    std::unordered_map<uint64_t, Slot> m_entries;
    std::unordered_map<int64_t, std::vector<uint64_t>> m_grid; ///< XZ cell -> entry ids
    float m_cellSize = 16.0f;
    std::vector<uint64_t> m_promotedIds;
    std::unordered_map<uint64_t, ProxyEntry> m_baseline; ///< the level's entries (taken at start())
    bool m_hasBaseline = false;
    uint64_t m_nextId = 1;
    glm::vec3 m_scanFocus{1e30f};
    bool m_rescan = true;
    mutable ProxyStats m_stats;
};

class TypeRegistry;
/// Reflection (scene files, editor): ProxySet. The Engine calls it at startup.
void registerProxyTypes(TypeRegistry& r);

} // namespace ve
