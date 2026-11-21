# World streaming for placed objects (proposal)

**Status:** proposal for review. Nothing here is built yet. It builds on static
renderers (`MeshRenderer::isStatic`) and proxy sets (`ve::ProxySet`).

## Goal

Terrain, foliage and grass already stream around the player. Objects placed in
the editor do not: every hut, chest, ruin and spawner in the scene file exists
for the whole session, with its components, colliders, sounds and behaviour.
That's fine for one camp, but not for a map with dozens of villages and
points of interest.

With streaming, a placed object can belong to a **cell** and only exist as a
real GameObject while a player is close enough. Further away it is data, or a
cheap far-away look.

**Not goals (for now):** streaming from disk (the scene file is small: it's
loaded whole and kept in memory as JSON), and simulating unloaded areas.

## The model

```
scene file ──load──► WorldStreamer: cells (e.g. 128 m) ──► records (object JSON + saved state)
                                     │
             player within loadDistance ▼                    ▲ beyond loadDistance × 1.25
                            real GameObjects ── unload: capture state ──┘
                     (beyond loadDistance: optional far proxy in a ProxySet)
```

- **Per object** (a `Streaming` component or fields on the root object):
  - `mode`: `AlwaysLoaded` (the default, today's behaviour), or `Streamed`.
  - `loadDistance`: when its real objects exist (default from the streamer,
    e.g. 150 m).
  - `farLook`: none, or a far proxy drawn while unloaded up to `viewDistance`.
    This covers the "mountain" case: you see the big rock formation from
    kilometres away, but its colliders and anything on it only spawn up close.
- **Cell** = the object's position when the scene loads, or when it unloads (so
  things that moved, like a pushed cart, go to the cell they're in now). An
  explicit cell override is possible but rarely needed.
- **Whole prefab subtrees stream together**: the root's setting decides.
  Children don't stream on their own.

## Loading and unloading

- The `WorldStreamer` (one per scene) gets the root records of all streamed
  objects at scene load, instead of creating them.
- Every few frames it compares each cell against the **focus points** (the
  camera, every player character; on a host, every connected player's) and:
  - **loads** records within `loadDistance`: `SceneSerializer::createObject`
    from the stored JSON with the original persistent ids, then `loadState()`
    with any state the record carries;
  - **unloads** objects beyond `loadDistance × hysteresis`: captures every
    `Saveable`'s state and the transform into the record, then destroys the
    object.
- Both have a per-frame budget (milliseconds or objects); nearest first.
- **Loading screens**: the streamer registers a load task that waits for the
  cells around the starting point (like foliage's `readyDistance`).
- Objects that are **destroyed by gameplay** while loaded (a chest broken
  apart) mark their record removed, so they never come back.
- **Ids stay stable**: a record always recreates the same persistent ids, so
  `ObjectRef`s to a streamed object resolve again once it reloads (and are
  null while it's unloaded, which code must already handle).

## Interaction with the rest of the engine

| Area | How it works with streaming |
|---|---|
| **Save games** | The save captures state the same way: loaded objects as today; unloaded ones from their records (the streamer is a `Saveable` that writes each record's state under the object's usual key). `removed` includes removed records. Load applies state to records, or to objects if loaded. |
| **Editor** | In Edit mode **everything loads** (streaming paused), so selection, gizmos and saving the scene stay exactly as today. Play mode streams. A "preview streaming" toggle shows cell bounds and load circles. Large worlds that can't load at once in the editor: later, by saving from records. |
| **Static renderers / proxies** | A streamed object's far look is an entry in a `ProxySet` (its static meshes, merged or as LOD chains / impostors), hidden while the object is loaded. |
| **Physics** | Colliders exist only while loaded. Dynamic bodies near the edge of the loaded area: a body is unloaded with its cell, so nothing falls into an unloaded area while it simulates. |
| **Navigation** | Nav tiles are built from loaded colliders, and agents only exist where things are loaded. Later: keep static collider triangles in records so far tiles are right too. |
| **Multiplayer (D19)** | Only the host streams: it keeps cells loaded around every player, and clients get the usual spawn / despawn replication. This is the first step of the roadmap's interest management (later: only send each client what's near it). |
| **AI** | An agent walking out of the loaded area unloads with its cell (its state saved) and resumes when the area loads again. Long-distance AI (a trader travelling between villages) would need a separate, data-only simulation; out of scope. |
| **Audio** | Sources and zones on streamed objects come and go with them. Loop starts use the same fade as today. |

## Phases

1. **Core:** `Streaming` settings, `WorldStreamer` with cells, records, budgeted
   load/unload, state round-trip, removal, the load task, Edit-mode load-all,
   save integration. Savanna: a few remote points of interest (an abandoned
   kraal, a watering-hole camp) set to Streamed; the autotest walks away,
   checks they unloaded with their state, walks back, checks it's restored, and
   saves/loads in both states.
2. **Far looks:** a `ProxySet` far proxy per streamed object (generated from its
   static meshes, impostor where it helps).
3. **Multiplayer:** focus points for every player on the host; replication
   follows load/unload.
4. **Navigation:** colliders kept in records for far nav tiles.
5. **Big worlds in the editor:** stream in Edit mode, save from records.

## Questions to settle before building

1. **Default cell size and load distance.** I'd start with 128 m cells and
   150 m loads: big enough that a village is one or two cells and nothing pops
   up close.
2. **What streams by default?** I'd make `AlwaysLoaded` the default so nothing
   changes until a scene opts objects in. Alternatively: stream everything that
   has no behaviour you'd miss (static props).
3. **Is phase 3 (multiplayer) needed in the first round,** or is single-player
   plus host-only streaming (clients see what the host has loaded) acceptable
   for now?
