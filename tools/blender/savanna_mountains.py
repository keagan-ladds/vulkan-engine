#!/usr/bin/env python3
"""
Savanna mountains: a procedural Blender generator for large backdrop landforms
(sandstone mesas and buttes, jagged ridges, granite kopjes) with a LOD chain.

How a mountain is built:
  1. Heightfield: a plan outline (squared-off ellipse, bent by noise into bays,
     promontories and buttresses) and a height profile across it: domed,
     cragged summit -> rounded rim -> one or more near-vertical cliff tiers
     with undulating benches -> concave talus apron -> ground, sinking a few
     metres below 0 at the edges so it beds into terrain.
  2. Fallen boulders (icospheres bent by noise) on the talus, or stacked on top
     for kopjes, joined to the heightfield as closed solids.
  3. Voxel remesh: everything is unioned into one surface with even triangles,
     so the vertical cliffs get as much detail as the flat ground does
     (a plain heightfield would stretch them into slivers). The parts below
     ground are cut away.
  4. 3D detail displaced along the normals: horizontal sandstone strata with
     overhanging lips, vertical erosion flutes on the cliffs, rock noise.
  5. Vertex colours (COLOR_0): banded rock, dark water streaks on the cliffs,
     dry scrub on flat ground, darker crevices.
  6. LODs: each level decimated from the dense mesh to a triangle budget.
     LOD1+ take their normals from LOD0, so lighting barely changes when they
     swap. Box-projected UVs in metres (for tiling rock textures).
  7. Export one glTF/GLB with the objects <Name>_LOD0, <Name>_LOD1, ...
     (+Y up, +optional <Name>_COL collision proxy), and optionally a .blend
     and a Cycles preview render.

Command line (tested with Blender 5.3; written to also run on 3.6+; or the
`bpy` pip module):
    blender --background --python tools/blender/savanna_mountains.py -- \
        --preset mesa --seed 7 --out mountain_mesa.glb --preview mesa.png

    # every preset, 3 seeds each, into a folder
    blender -b -P tools/blender/savanna_mountains.py -- --preset all --count 3 --out-dir out/

    # options: --help (after the "--")

Inside Blender: open this file in the Text Editor, edit UI_CONFIG below and
press Run Script. The mountain appears in the scene (nothing is exported
unless UI_CONFIG has "out").

Sizes are metres, Blender's Z is up (the glTF exporter converts to +Y up).
"""

import argparse
import math
import os
import sys
import time

import bpy  # before bmesh / mathutils (needed by the bpy pip module)
import bmesh
import mathutils
import numpy as np

# Settings used when the script is run from Blender's Text Editor.
UI_CONFIG = {
    "preset": "mesa",
    "seed": 7,
    "lods": None,          # e.g. [160000, 50000, 16000, 5000]; None = the preset's
    "collision": False,
    "side_by_side": True,  # lay the LODs out next to each other for inspection
    "out": "",             # e.g. "//mountain_mesa.glb" (// = next to the .blend)
}

# =============================================================================
# Presets. Outline frequencies are in cycles per radius; surface detail
# frequencies in cycles per metre. Override any of them with --set key=value.
# =============================================================================

BASE = dict(
    # --- size / plan shape ---
    radius=230.0,         # half-width of the cliff outline across Y (m)
    elongation=1.3,       # X / Y stretch of the outline
    height=300.0,         # summit height above the ground (m)
    squareness=2.6,       # superellipse exponent: 2 = ellipse, higher = blockier
    warp=0.08,            # domain warp of the outline (fraction of radius)
    lobe_amp=0.45,        # big bays and promontories
    lobe_freq=1.6,
    gully_amp=0.22,       # buttresses and chimneys along the cliff line
    gully_freq=8.0,
    # --- profile ---
    cliff_width=0.05,     # horizontal width of a cliff band (fraction of radius); small = vertical
    tiers=2,              # stacked cliff bands with benches between (Drakensberg style)
    tier_gap=0.16,        # how far each upper tier is set back (fraction of radius)
    rim_round=0.05,       # how far the cliff-top shoulders are rounded down (fraction of height)
    rim_width=0.10,
    dome=0.18,            # summit dome (fraction of height)
    crown_amp=0.22,       # jagged crags / peaks on top (fraction of height)
    crown_freq=3.0,
    crown_reach=0.0,      # 0 = crags only on the top; 1 = all the way down the cliffs / flanks
    talus_height=0.22,    # scree apron height at the cliff foot (fraction of height)
    talus_width=0.6,      # how far the apron runs out (fraction of radius)
    talus_exp=2.0,        # concavity of the apron
    ground_noise=3.0,     # bumps on the apron and the ground (m)
    sink=6.0,             # edges end this far below z = 0
    # --- surface detail (metres) ---
    strata_spacing=15.0,  # thickness of the rock layers
    strata_depth=2.6,     # how far ledges stick out
    strata_tilt=(0.03, 0.012),
    strata_warp=4.0,      # layers undulate by this much
    flute_amp=4.0,        # vertical erosion ribs on cliffs
    flute_freq=0.035,
    detail_amp=1.0,       # rock noise everywhere
    detail_freq=0.07,
    # --- boulders ---
    boulders=40,
    boulder_size=(4.0, 13.0),   # radius range (m)
    boulder_zone=(1.0, 1.4),    # where, in outline units (1 = cliff foot)
    boulder_sink=0.35,          # fraction of the boulder's height below the surface
    # --- colour ---
    palette="sandstone",
    vegetation=0.5,       # amount of scrub on flat ground (0..1)
    # --- output ---
    lods=(160000, 50000, 16000, 5000),   # triangles per level
    collision_tris=1500,
    oversample=2.5,       # remesh density relative to LOD0 before decimating
    uv_scale=4.0,         # metres per UV unit (box projection)
)

PRESETS = {
    # The hero landmark: a sandstone massif with two tiers of sheer banded
    # cliffs, a domed, cragged summit and a scree apron (Drakensberg / Waterberg).
    "mesa": {},
    # A single, smaller stack.
    "butte": dict(radius=95.0, elongation=1.15, height=150.0, squareness=2.2, lobe_amp=0.18,
                  gully_amp=0.1, gully_freq=6.0, tiers=1, cliff_width=0.07, crown_amp=0.06, dome=0.1,
                  talus_height=0.3, talus_width=0.8, strata_spacing=10.0, strata_depth=1.8,
                  flute_amp=2.5, flute_freq=0.05, boulders=25, boulder_size=(2.5, 8.0),
                  lods=(80000, 25000, 8000, 2500)),
    # A long, jagged range for the far distance.
    "ridge": dict(radius=360.0, elongation=3.2, height=240.0, squareness=2.0, lobe_amp=0.3,
                  gully_amp=0.14, gully_freq=9.0, tiers=1, cliff_width=0.6, rim_round=0.0, dome=0.7,
                  crown_amp=0.75, crown_freq=4.5, crown_reach=0.85, talus_height=0.1, talus_width=0.4,
                  strata_spacing=18.0, strata_depth=1.4, flute_amp=4.5, boulders=0,
                  vegetation=0.3, lods=(160000, 50000, 16000, 5000)),
    # A granite kopje: a low dome of piled, rounded boulders.
    "kopje": dict(radius=40.0, elongation=1.3, height=16.0, squareness=2.0, lobe_amp=0.25,
                  gully_amp=0.0, tiers=1, cliff_width=0.75, rim_round=0.0, dome=0.5, crown_amp=0.1,
                  crown_freq=2.0, talus_height=0.15, talus_width=0.4, ground_noise=0.5, sink=2.0,
                  strata_depth=0.0, flute_amp=0.0, detail_amp=0.45, detail_freq=0.22,
                  boulders=48, boulder_size=(4.0, 14.0), boulder_zone=(0.0, 0.95), boulder_sink=0.22,
                  palette="granite", vegetation=0.35, lods=(40000, 12000, 4000, 1200),
                  collision_tris=600, uv_scale=2.0),
}

PALETTES = {  # sRGB 0..255
    "sandstone": dict(
        rock=(204, 130, 66), pale=(226, 186, 128), dark=(160, 88, 50),
        streak=(96, 74, 58), crevice=(70, 46, 32), scrub=(150, 130, 70), bush=(48, 60, 28),
        soil=(178, 124, 78)),
    "granite": dict(
        rock=(156, 144, 128), pale=(180, 170, 152), dark=(124, 112, 98),
        streak=(72, 68, 62), crevice=(54, 50, 46), scrub=(140, 120, 70), bush=(52, 62, 30),
        soil=(170, 126, 84)),
}


# =============================================================================
# Noise (vectorised numpy Perlin; deterministic for a seed, no Blender noise)
# =============================================================================

_GRAD = np.array([[1, 1, 0], [-1, 1, 0], [1, -1, 0], [-1, -1, 0], [1, 0, 1], [-1, 0, 1], [1, 0, -1], [-1, 0, -1],
                  [0, 1, 1], [0, -1, 1], [0, 1, -1], [0, -1, -1], [1, 1, 0], [0, -1, 1], [-1, 1, 0], [0, -1, -1]],
                 dtype=np.float64)


class Noise:
    def __init__(self, seed):
        rng = np.random.default_rng(seed)
        p = rng.permutation(256).astype(np.int64)
        self.p = np.concatenate([p, p])
        self.offsets = rng.uniform(-500.0, 500.0, size=(32, 3))

    def perlin(self, x, y, z):
        """Gradient noise, roughly -1..1."""
        xi, yi, zi = np.floor(x), np.floor(y), np.floor(z)
        xf, yf, zf = x - xi, y - yi, z - zi
        X = xi.astype(np.int64) & 255
        Y = yi.astype(np.int64) & 255
        Z = zi.astype(np.int64) & 255
        u = xf * xf * xf * (xf * (xf * 6 - 15) + 10)
        v = yf * yf * yf * (yf * (yf * 6 - 15) + 10)
        w = zf * zf * zf * (zf * (zf * 6 - 15) + 10)
        p = self.p
        A = p[X] + Y
        B = p[X + 1] + Y
        AA, AB, BA, BB = p[A] + Z, p[A + 1] + Z, p[B] + Z, p[B + 1] + Z

        def g(h, dx, dy, dz):
            gr = _GRAD[p[h] & 15]
            return gr[..., 0] * dx + gr[..., 1] * dy + gr[..., 2] * dz

        def lerp(t, a, b):
            return a + t * (b - a)

        x1 = lerp(u, g(AA, xf, yf, zf), g(BA, xf - 1, yf, zf))
        x2 = lerp(u, g(AB, xf, yf - 1, zf), g(BB, xf - 1, yf - 1, zf))
        x3 = lerp(u, g(AA + 1, xf, yf, zf - 1), g(BA + 1, xf - 1, yf, zf - 1))
        x4 = lerp(u, g(AB + 1, xf, yf - 1, zf - 1), g(BB + 1, xf - 1, yf - 1, zf - 1))
        return lerp(w, lerp(v, x1, x2), lerp(v, x3, x4))

    def fbm(self, x, y, z, octaves=4, lacunarity=2.0, gain=0.5, layer=0):
        """Fractal noise, roughly -1..1."""
        total, amp, norm, f = 0.0, 1.0, 0.0, 1.0
        for o in range(octaves):
            off = self.offsets[(layer * 5 + o) % len(self.offsets)]
            total = total + amp * self.perlin(x * f + off[0], y * f + off[1], z * f + off[2])
            norm += amp
            amp *= gain
            f *= lacunarity
        return total / norm

    def ridged(self, x, y, z, octaves=4, lacunarity=2.1, gain=0.5, layer=0):
        """Ridged multifractal, 0..1 with sharp crests near 1."""
        total, amp, norm, f = 0.0, 1.0, 0.0, 1.0
        weight = 1.0
        for o in range(octaves):
            off = self.offsets[(layer * 7 + o + 11) % len(self.offsets)]
            n = 1.0 - np.abs(self.perlin(x * f + off[0], y * f + off[1], z * f + off[2]))
            n = n * n * weight
            weight = np.clip(n * 1.5, 0.0, 1.0)
            total = total + amp * n
            norm += amp
            amp *= gain
            f *= lacunarity
        return total / norm


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def srgb_to_linear(c):
    c = np.asarray(c, dtype=np.float64) / 255.0
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


# =============================================================================
# 1. Heightfield
# =============================================================================

class Heightfield:
    """The mountain's base shape as z = f(x, y), plus its outline distance."""

    def __init__(self, P, noise):
        self.P, self.n = P, noise

    def outline(self, x, y):
        """Normalised distance to the cliff foot: < 1 inside the massif."""
        P, n, R = self.P, self.n, self.P["radius"]
        wx = x + P["warp"] * R * n.fbm(x / R * 1.3, y / R * 1.3, 0.5, 3, layer=1)
        wy = y + P["warp"] * R * n.fbm(x / R * 1.3, y / R * 1.3, 7.5, 3, layer=2)
        u = np.abs(wx / (R * P["elongation"]))
        v = np.abs(wy / R)
        q = P["squareness"]
        r = (u ** q + v ** q) ** (1.0 / q)
        lobes = P["lobe_amp"] * n.fbm(x / R * P["lobe_freq"], y / R * P["lobe_freq"], 3.1, 3, layer=3)
        gully = P["gully_amp"] * (n.ridged(x / R * P["gully_freq"], y / R * P["gully_freq"], 9.7, 3, layer=4) - 0.45)
        return r / np.maximum(1.0 + lobes + gully, 0.2)

    def reach(self):
        """Outline units (d) at which the mesh ends, and the same in metres along Y."""
        P = self.P
        d_end = 1.0 + P["talus_width"] + 0.22
        return d_end, P["radius"] * (d_end * (1.0 + P["lobe_amp"] + 0.6 * P["gully_amp"]) + P["warp"])

    def height(self, x, y):
        P, n, R, H = self.P, self.n, self.P["radius"], self.P["height"]
        d = self.outline(x, y)
        cw = P["cliff_width"]
        tiers = max(1, int(P["tiers"]))

        # Scree apron: highest under the cliff, concave out to the ground.
        t = np.clip((d - 1.0) / P["talus_width"], 0.0, 1.0)
        apron = H * P["talus_height"] * (1.0 - t) ** P["talus_exp"]
        apron = apron * (1.0 + 0.25 * n.fbm(x / R * 4, y / R * 4, 5.3, 3, layer=6))

        # Cliff tiers: each one a near-vertical step with its own wobbly
        # outline, the inner ones set back by tier_gap with a bench between.
        levels = list(np.linspace(P["talus_height"], 1.0, tiers + 1))
        for i in range(1, tiers):  # benches rise and fall instead of sitting level
            levels[i] = levels[i] + (levels[i + 1] - levels[i - 1]) * 0.22 * n.fbm(
                x / R * 1.5, y / R * 1.5, 21.0 + i, 3, layer=24 + i)
        z = apron.copy()
        d_top = 1.0
        for i in range(tiers):
            di = 1.0 - i * P["tier_gap"]
            if i > 0:
                di = di + P["tier_gap"] * 0.8 * n.fbm(x / R * 3, y / R * 3, 11.0 + i, 3, layer=20 + i)
            w = smoothstep(di, di - cw, d)
            z = z + H * (levels[i + 1] - levels[i]) * w
            d_top = di - cw
        # Rounded shoulders at the top rim.
        z = z - H * P["rim_round"] * smoothstep(d_top - P["rim_width"], d_top, d) ** 2 * smoothstep(d_top + cw, d_top, d)

        # Summit: a dome over the top tier plus jagged crags.
        inner = smoothstep(d_top + P["crown_reach"] * (1.0 - d_top), 0.55 * d_top, d)
        dome = 1.0 - np.clip(d / np.maximum(d_top, 1e-3), 0.0, 1.0) ** 2
        z = z + H * P["dome"] * dome
        crags = n.ridged(x / R * P["crown_freq"], y / R * P["crown_freq"], 1.7, 5, layer=5)
        z = z + H * P["crown_amp"] * (crags - 0.35) * inner

        # Bumps on the apron and ground, fading out past the apron, where the
        # ground settles to `sink` below zero so the mesh beds into terrain.
        end = 1.0 + P["talus_width"]
        z = z + P["ground_noise"] * n.fbm(x / R * 12, y / R * 12, 2.2, 4, layer=7) * smoothstep(end + 0.05, end - 0.15, d)
        z = z - P["sink"] * smoothstep(end - 0.05, end + 0.2, d)
        return z, d


def build_grid(P, hf, cell):
    """Grid extents, sample positions and heights (edges at -sink)."""
    _, hy = hf.reach()
    hx = hy - P["radius"] + P["radius"] * P["elongation"]
    hx = max(hx, hy * P["elongation"] * 0.9)
    nx, ny = max(16, int(2 * hx / cell)), max(16, int(2 * hy / cell))
    xs, ys = np.linspace(-hx, hx, nx + 1), np.linspace(-hy, hy, ny + 1)
    X, Y = np.meshgrid(xs, ys)  # shape (ny+1, nx+1)
    Z, D = hf.height(X, Y)
    # Whatever reaches the grid border is pulled down to -sink.
    border = np.minimum(np.minimum(X + hx, hx - X) / hx, np.minimum(Y + hy, hy - Y) / hy)
    fade = smoothstep(0.0, 0.06, border)
    Z = Z * fade - P["sink"] * (1.0 - fade)
    return xs, ys, X, Y, Z, D


def grid_surface_area(X, Y, Z, above):
    """Surface area of the cells whose corners are all above `above`."""
    p00 = np.stack([X[:-1, :-1], Y[:-1, :-1], Z[:-1, :-1]], -1)
    p10 = np.stack([X[:-1, 1:], Y[:-1, 1:], Z[:-1, 1:]], -1)
    p01 = np.stack([X[1:, :-1], Y[1:, :-1], Z[1:, :-1]], -1)
    p11 = np.stack([X[1:, 1:], Y[1:, 1:], Z[1:, 1:]], -1)
    a = 0.5 * np.linalg.norm(np.cross(p10 - p00, p11 - p00), axis=-1)
    b = 0.5 * np.linalg.norm(np.cross(p11 - p00, p01 - p00), axis=-1)
    keep = (Z[:-1, :-1] > above) & (Z[:-1, 1:] > above) & (Z[1:, :-1] > above) & (Z[1:, 1:] > above)
    return float(((a + b) * keep).sum())


def heightfield_solid(X, Y, Z, bottom):
    """Closed mesh: the grid on top, walls and a flat bottom at `bottom`."""
    ny1, nx1 = Z.shape
    top = np.stack([X.ravel(), Y.ravel(), Z.ravel()], -1)
    idx = np.arange(ny1 * nx1).reshape(ny1, nx1)
    a, b, c, d = idx[:-1, :-1], idx[:-1, 1:], idx[1:, 1:], idx[1:, :-1]  # CCW seen from above
    faces = np.stack([a.ravel(), b.ravel(), c.ravel(), d.ravel()], -1).tolist()
    # Boundary loop, counter-clockwise from above.
    ring = np.concatenate([idx[0, :], idx[1:, -1], idx[-1, -2::-1], idx[-2:0:-1, 0]])
    base = len(top)
    low = top[ring].copy()
    low[:, 2] = bottom
    verts = np.concatenate([top, low])
    m = len(ring)
    for k in range(m):
        k1 = (k + 1) % m
        faces.append([int(ring[k1]), int(ring[k]), base + k, base + k1])
    faces.append([base + k for k in range(m - 1, -1, -1)])  # bottom, facing down
    return verts, faces


# =============================================================================
# 2. Boulders
# =============================================================================

def make_boulders(P, X, Y, Z, D, noise, rng):
    """Closed, noisy ellipsoids sitting partly sunk into the surface."""
    count = int(P["boulders"])
    if count <= 0:
        return [], []
    lo, hi = P["boulder_zone"]
    cand = np.argwhere((D >= lo) & (D <= hi) & (Z > 0.0))
    if len(cand) == 0:
        return [], []
    bm = bmesh.new()
    bmesh.ops.create_icosphere(bm, subdivisions=3, radius=1.0)
    unit = np.array([v.co[:] for v in bm.verts])
    tris = [[v.index for v in f.verts] for f in bm.faces]
    bm.free()

    verts, faces, base = [], [], 0
    picks = cand[rng.choice(len(cand), size=min(count, len(cand)), replace=False)]
    smin, smax = P["boulder_size"]
    for k, (j, i) in enumerate(picks):
        r = smin + (smax - smin) * rng.random() ** 1.8  # mostly small, a few big ones
        scale = r * np.array([1.0 + 0.35 * rng.random(), 0.75 + 0.35 * rng.random(), 0.55 + 0.35 * rng.random()])
        bump = 1.0 + 0.3 * noise.fbm(unit[:, 0] * 1.6 + k * 3.1, unit[:, 1] * 1.6, unit[:, 2] * 1.6, 3, layer=8)
        p = unit * bump[:, None]
        # Rocks break along planes: cut the blob with a few random planes.
        for _ in range(4):
            nrm = rng.normal(size=3)
            nrm /= np.linalg.norm(nrm)
            depth = rng.uniform(0.55, 0.85)
            over = p @ nrm - depth
            p -= np.maximum(over, 0.0)[:, None] * nrm[None, :] * 0.9
        p *= scale
        a, b = rng.uniform(0, 2 * math.pi), rng.uniform(-0.35, 0.35)
        ca, sa, cb, sb = math.cos(a), math.sin(a), math.cos(b), math.sin(b)
        rot = np.array([[ca, -sa, 0], [sa, ca, 0], [0, 0, 1]]) @ np.array([[1, 0, 0], [0, cb, -sb], [0, sb, cb]])
        p = p @ rot.T
        x, y = X[j, i] + rng.uniform(-1, 1), Y[j, i] + rng.uniform(-1, 1)
        z = Z[j, i] + scale[2] * (1.0 - 2.0 * P["boulder_sink"])
        verts.append(p + np.array([x, y, z]))
        faces.extend([[base + t for t in tri] for tri in tris])
        base += len(p)
    return np.concatenate(verts), faces


# =============================================================================
# Blender helpers (version tolerant; tested on 5.3)
# =============================================================================

def link_new_object(name, mesh, collection):
    obj = bpy.data.objects.new(name, mesh)
    collection.objects.link(obj)
    return obj


def apply_modifier(obj, mod_type, name="mod", **settings):
    """Adds a modifier and bakes it into the mesh (no bpy.ops context needed)."""
    mod = obj.modifiers.new(name, mod_type)
    for k, v in settings.items():
        setattr(mod, k, v)
    dg = bpy.context.evaluated_depsgraph_get()
    dg.update()
    replace_mesh(obj, bpy.data.meshes.new_from_object(obj.evaluated_get(dg), depsgraph=dg))
    return obj


def replace_mesh(obj, new_mesh):
    old = obj.data
    name = old.name
    obj.modifiers.clear()
    obj.data = new_mesh
    bpy.data.meshes.remove(old)
    new_mesh.name = name


def mesh_coords(mesh):
    co = np.empty(len(mesh.vertices) * 3)
    mesh.vertices.foreach_get("co", co)
    return co.reshape(-1, 3)


def vertex_normals(mesh):
    nrm = np.empty(len(mesh.vertices) * 3)
    if hasattr(mesh, "vertex_normals"):  # 3.5+
        mesh.vertex_normals.foreach_get("vector", nrm)
    else:
        mesh.vertices.foreach_get("normal", nrm)
    return nrm.reshape(-1, 3)


def use_nodes(idblock):
    if bpy.app.version < (5, 0, 0):  # always on (and deprecated) from 5.0
        idblock.use_nodes = True


def triangle_count(mesh):
    mesh.calc_loop_triangles()
    return len(mesh.loop_triangles)


def set_smooth(mesh):
    mesh.polygons.foreach_set("use_smooth", [True] * len(mesh.polygons))
    if hasattr(mesh, "use_auto_smooth"):  # < 4.1: needed for custom normals
        mesh.use_auto_smooth = True
        mesh.auto_smooth_angle = math.pi
    mesh.update()


def delete_below(obj, z):
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    doomed = [v for v in bm.verts if v.co.z < z]
    bmesh.ops.delete(bm, geom=doomed, context="VERTS")
    # Loose bits (bits of boulders that ended up separate) would just waste triangles.
    loose = [v for v in bm.verts if not v.link_faces]
    bmesh.ops.delete(bm, geom=loose, context="VERTS")
    bm.to_mesh(obj.data)
    bm.free()
    obj.data.update()


def remove_small_islands(obj, min_faces):
    """Drops disconnected pieces with fewer than `min_faces` faces (remesh crumbs)."""
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    bm.faces.ensure_lookup_table()
    seen = set()
    doomed = []
    for f in bm.faces:
        if f.index in seen:
            continue
        island, stack = [], [f]
        seen.add(f.index)
        while stack:
            cur = stack.pop()
            island.append(cur)
            for e in cur.edges:
                for g in e.link_faces:
                    if g.index not in seen:
                        seen.add(g.index)
                        stack.append(g)
        if len(island) < min_faces:
            doomed.extend(island)
    if doomed:
        bmesh.ops.delete(bm, geom=doomed, context="FACES")
        bm.to_mesh(obj.data)
    bm.free()


# =============================================================================
# 4. Surface detail and 5. colours
# =============================================================================

def displace_detail(P, mesh, noise):
    co = mesh_coords(mesh)
    nr = vertex_normals(mesh)
    x, y, z = co[:, 0], co[:, 1], co[:, 2]
    nz = nr[:, 2]
    steep = smoothstep(0.75, 0.3, nz)  # 1 on cliffs

    # Strata: horizontal bands, gently tilted and undulating. Within a band
    # the face pushes out towards the top, then steps back: overhanging lips.
    tx, ty = P["strata_tilt"]
    sz = z + tx * x + ty * y + P["strata_warp"] * noise.fbm(x * 0.006, y * 0.006, z * 0.002, 3, layer=9)
    band = sz / P["strata_spacing"]
    k = np.floor(band)
    f = band - k
    band_amp = 0.55 + 0.45 * noise.perlin(k * 0.71 + 0.3, 3.7, 1.9) * 2.0
    ledge = (f ** 2.5 - 0.3) * np.clip(band_amp, 0.1, 1.4)
    disp = P["strata_depth"] * ledge * steep

    # Vertical flutes: noise stretched along Z, ridged so the ribs are sharp.
    ff = P["flute_freq"]
    flutes = noise.ridged(x * ff, y * ff, z * ff * 0.12, 3, layer=10) - 0.5
    disp += P["flute_amp"] * flutes * steep

    # Rock noise everywhere, gentler on flat ground.
    df = P["detail_freq"]
    rock = noise.fbm(x * df, y * df, z * df, 4, layer=11)
    disp += P["detail_amp"] * rock * (0.45 + 0.55 * steep)

    co += nr * disp[:, None]
    mesh.vertices.foreach_set("co", co.ravel())
    mesh.update()
    return dict(steep=steep, nz=nz, band=band, disp=disp, z0=z)


def paint_colors(P, mesh, noise, info):
    pal = PALETTES[P["palette"]]
    co = mesh_coords(mesh)
    x, y, z = co[:, 0], co[:, 1], co[:, 2]
    steep, nz, band, disp = info["steep"], info["nz"], info["band"], info["disp"]
    H = P["height"]

    # Rock: each band a little lighter or darker, the odd one pale or rusty,
    # blended across band boundaries.
    def band_tint(kk):
        h = (np.sin(kk * 12.9898 + P["strata_spacing"]) * 43758.5453) % 1.0  # per-band hash
        shade = 0.86 + 0.26 * ((h * 7.31) % 1.0)
        pale = smoothstep(0.9, 0.95, h)[:, None] * 0.5
        dark = smoothstep(0.16, 0.1, h)[:, None] * 0.6
        c = srgb_to_linear(pal["rock"]) * shade[:, None]
        c = c * (1 - pale) + srgb_to_linear(pal["pale"]) * pale
        return c * (1 - dark) + srgb_to_linear(pal["dark"]) * dark

    k = np.floor(band)
    f = (band - k)[:, None]
    blend = smoothstep(0.7, 1.0, f)
    rock = band_tint(k) * (1 - blend) + band_tint(k + 1) * blend
    rock *= (0.88 + 0.24 * noise.fbm(x * 0.02, y * 0.02, z * 0.05, 3, layer=12))[:, None]

    # Water / lichen streaks running down the cliffs.
    streak = noise.fbm(x * 0.05, y * 0.05, z * 0.004, 3, layer=13)
    streak = smoothstep(0.05, 0.45, streak) * steep * 0.75
    col = rock * (1 - streak[:, None]) + srgb_to_linear(pal["streak"]) * streak[:, None]

    # Crevices (pushed in by the detail pass) are darker.
    cav = smoothstep(0.0, -max(P["strata_depth"], P["flute_amp"], P["detail_amp"], 0.5), disp) * 0.6
    col = col * (1 - cav[:, None]) + srgb_to_linear(pal["crevice"]) * cav[:, None]

    # Soil near the ground, dust on low rock.
    low = smoothstep(0.18 * H, 0.0, z) * (1 - steep)
    col = col * (1 - low[:, None]) + srgb_to_linear(pal["soil"]) * low[:, None]

    # Dry scrub on flat-ish ground in patches, dark bushes dotted through it.
    flat = smoothstep(0.65, 0.92, nz)
    v = P["vegetation"]
    patches = noise.fbm(x * 0.02, y * 0.02, z * 0.02, 4, layer=14) * 0.5 + 0.5
    scrub = flat * smoothstep(0.75 - 0.6 * v, 0.9 - 0.6 * v, patches)
    dots = noise.fbm(x * 0.12, y * 0.12, z * 0.12, 3, layer=15) * 0.5 + 0.5
    bush = flat * smoothstep(0.78 - 0.35 * v, 0.86 - 0.35 * v, dots) * (0.35 + 0.65 * scrub)
    col = col * (1 - scrub[:, None] * 0.8) + srgb_to_linear(pal["scrub"]) * (scrub[:, None] * 0.8)
    col = col * (1 - bush[:, None]) + srgb_to_linear(pal["bush"]) * bush[:, None]

    rgba = np.concatenate([np.clip(col, 0, 1), np.ones((len(col), 1))], 1)
    attr = mesh.color_attributes.new("Color", "BYTE_COLOR", "POINT")
    attr.data.foreach_set("color", rgba.astype(np.float32).ravel())
    mesh.color_attributes.active_color = attr
    if hasattr(mesh.color_attributes, "render_color_index"):
        mesh.color_attributes.render_color_index = mesh.color_attributes.find("Color")


def box_uvs(mesh, metres_per_uv):
    """Box projection by face normal, in world metres (tiling textures)."""
    if not mesh.uv_layers:
        mesh.uv_layers.new(name="UVMap")
    uv = mesh.uv_layers.active
    co = mesh_coords(mesh)
    nl = len(mesh.loops)
    lv = np.empty(nl, dtype=np.int64)
    mesh.loops.foreach_get("vertex_index", lv)
    pn = np.empty(len(mesh.polygons) * 3)
    mesh.polygons.foreach_get("normal", pn)
    pn = pn.reshape(-1, 3)
    start = np.empty(len(mesh.polygons), dtype=np.int64)
    total = np.empty(len(mesh.polygons), dtype=np.int64)
    mesh.polygons.foreach_get("loop_start", start)
    mesh.polygons.foreach_get("loop_total", total)
    loop_poly = np.repeat(np.arange(len(mesh.polygons)), total)
    n = pn[loop_poly]
    p = co[lv] / metres_per_uv
    axis = np.argmax(np.abs(n), axis=1)
    out = np.where(axis[:, None] == 0, p[:, [1, 2]], np.where(axis[:, None] == 1, p[:, [0, 2]], p[:, [0, 1]]))
    # Mirror so textures aren't flipped on the negative-facing sides.
    sgn = np.sign(n[np.arange(nl), axis])
    out[:, 0] *= np.where(axis == 2, 1.0, sgn)
    uv.data.foreach_set("uv", out.astype(np.float32).ravel())


def make_material(name):
    mat = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    use_nodes(mat)
    nt = mat.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled")
    vcol = nt.nodes.new("ShaderNodeVertexColor")
    vcol.layer_name = "Color"
    bsdf.inputs["Roughness"].default_value = 0.92
    nt.links.new(vcol.outputs["Color"], bsdf.inputs["Base Color"])
    nt.links.new(bsdf.outputs["BSDF"], out.inputs["Surface"])
    vcol.location, bsdf.location, out.location = (-400, 0), (-150, 0), (200, 0)
    return mat


def transfer_normals(src, dst):
    """Gives `dst` the shading of `src` (custom split normals)."""
    try:
        mod = dst.modifiers.new("normals", "DATA_TRANSFER")
        mod.object = src
        mod.use_loop_data = True
        mod.data_types_loops = {"CUSTOM_NORMAL"}
        mod.loop_mapping = "POLYINTERP_NEAREST"
        dg = bpy.context.evaluated_depsgraph_get()
        dg.update()
        replace_mesh(dst, bpy.data.meshes.new_from_object(dst.evaluated_get(dg), depsgraph=dg))
    except Exception as e:  # never fatal: the LOD just keeps its own normals
        dst.modifiers.clear()
        print(f"  (normal transfer skipped: {e})")


# =============================================================================
# The generator
# =============================================================================

def resolve_params(preset, overrides=None):
    P = dict(BASE)
    P.update(PRESETS[preset])
    P.update(overrides or {})
    return P


def generate(preset="mesa", seed=1, overrides=None, name=None, collection=None, collision=False,
             side_by_side=False, log=print):
    """Builds `<name>_LOD0..N` (+ `<name>_COL`) in `collection`. Returns the objects."""
    t0 = time.time()
    P = resolve_params(preset, overrides)
    name = name or f"{preset.capitalize()}_{seed:02d}"
    noise = Noise(seed)
    rng = np.random.default_rng(seed + 1000)
    if collection is None:
        collection = bpy.data.collections.new(name)
        bpy.context.scene.collection.children.link(collection)
    lods = [int(t) for t in P["lods"]]

    # --- 1. heightfield. A coarse pass sizes the voxels for the LOD0 budget. ---
    hf = Heightfield(P, noise)
    xs, ys, X, Y, Z, D = build_grid(P, hf, P["radius"] / 40.0)
    area = grid_surface_area(X, Y, Z, -P["sink"] * 0.9)  # what survives the cut below ground
    voxel = math.sqrt(area / (lods[0] * P["oversample"] / 2.0))
    cell = max(voxel * 0.9, 0.05)
    while (2 * xs[-1] / cell) * (2 * ys[-1] / cell) > 3.0e6:  # keep the grid sane
        cell *= 1.2
    xs, ys, X, Y, Z, D = build_grid(P, hf, cell)
    log(f"[{name}] heightfield {Z.shape[1]}x{Z.shape[0]}, surface {area / 1e6:.2f} km^2, voxel {voxel:.2f} m")

    bottom = -P["sink"] - max(20.0, P["height"] * 0.15)
    verts, faces = heightfield_solid(X, Y, Z, bottom)

    # --- 2. boulders ---
    bv, bf = make_boulders(P, X, Y, Z, D, noise, rng)
    if len(bv):
        off = len(verts)
        verts = np.concatenate([verts, bv])
        faces = faces + [[off + i for i in f] for f in bf]
        log(f"[{name}] {len(bf) // 320} boulders")

    mesh = bpy.data.meshes.new(name + "_src")
    mesh.from_pydata(verts.tolist(), [], faces)
    mesh.update()
    src = link_new_object(name + "_src", mesh, collection)

    # --- 3. voxel remesh: one even surface, boulders unioned in ---
    apply_modifier(src, "REMESH", mode="VOXEL", voxel_size=voxel, adaptivity=0.0)
    delete_below(src, -0.6 * P["sink"])  # keep a short buried skirt past the apron
    remove_small_islands(src, 200)
    apply_modifier(src, "SMOOTH", factor=0.5, iterations=2)
    log(f"[{name}] remeshed: {len(src.data.vertices)} verts ({time.time() - t0:.1f}s)")

    # --- 4/5. detail + colours on the dense mesh ---
    info = displace_detail(P, src.data, noise)
    paint_colors(P, src.data, noise, info)
    set_smooth(src.data)
    dense_tris = triangle_count(src.data)

    # --- 6. LODs ---
    mat = make_material(f"Mountain_{P['palette']}")
    objs = []
    spacing = 2.4 * P["radius"] * P["elongation"] * (1.0 + P["talus_width"])
    for i, target in enumerate(lods):
        obj = link_new_object(f"{name}_LOD{i}", src.data.copy(), collection)
        obj.data.name = obj.name
        ratio = min(1.0, target / dense_tris)
        apply_modifier(obj, "DECIMATE", decimate_type="COLLAPSE", ratio=ratio, use_collapse_triangulate=True)
        set_smooth(obj.data)
        box_uvs(obj.data, P["uv_scale"])
        if i > 0:
            transfer_normals(objs[0], obj)
        obj.data.materials.clear()
        obj.data.materials.append(mat)
        if side_by_side:
            obj.location.x = i * spacing
        objs.append(obj)
        log(f"[{name}] LOD{i}: {triangle_count(obj.data)} triangles (target {target})")

    if collision:
        col = link_new_object(f"{name}_COL", src.data.copy(), collection)
        col.data.name = col.name
        apply_modifier(col, "DECIMATE", decimate_type="COLLAPSE",
                       ratio=min(1.0, P["collision_tris"] / dense_tris), use_collapse_triangulate=True)
        col.data.color_attributes.remove(col.data.color_attributes["Color"])
        col.display_type = "WIRE"
        col.hide_render = True
        if side_by_side:
            col.location.x = -spacing
        objs.append(col)
        log(f"[{name}] COL: {triangle_count(col.data)} triangles")

    mesh = src.data
    bpy.data.objects.remove(src)
    bpy.data.meshes.remove(mesh)
    log(f"[{name}] done in {time.time() - t0:.1f}s")
    return objs


# =============================================================================
# Export and preview
# =============================================================================

def export_gltf(objs, path):
    os.makedirs(os.path.dirname(os.path.abspath(path)) or ".", exist_ok=True)
    bpy.context.view_layer.update()
    for o in bpy.context.scene.objects:
        o.select_set(False)
    saved = [(o, o.location.copy()) for o in objs]
    for o in objs:
        o.location = (0.0, 0.0, 0.0)  # every level at the origin in the file
        o.select_set(True)
    bpy.context.view_layer.objects.active = objs[0]
    want = dict(filepath=os.path.abspath(path),
                export_format="GLB" if path.lower().endswith(".glb") else "GLTF_SEPARATE",
                use_selection=True, export_apply=True, export_yup=True, export_normals=True,
                export_texcoords=True, export_materials="EXPORT",
                export_vertex_color="ACTIVE",  # 4.2+
                export_all_vertex_colors=False,
                export_colors=True,            # < 4.2
                export_tangents=False, export_animations=False, export_extras=False)
    have = bpy.ops.export_scene.gltf.get_rna_type().properties.keys()
    bpy.ops.export_scene.gltf(**{k: v for k, v in want.items() if k in have})
    for o, loc in saved:
        o.location = loc
    print(f"wrote {path}")


def render_preview(objs, path, width=1600):
    """Two Cycles (CPU) renders under a low afternoon sun: `path` shows the
    levels side by side (orthographic), `<path>_hero.png` LOD0 in perspective."""
    scene = bpy.context.scene
    lods = [o for o in objs if not o.name.endswith("_COL")]
    saved = [(o, o.location.copy(), o.hide_render) for o in objs]
    temp_objs, temp_data = [], []

    def add(obj):
        scene.collection.objects.link(obj)
        temp_objs.append(obj)
        temp_data.append(obj.data)
        return obj

    zmax = max(max(c[2] for c in o.bound_box) for o in lods)
    ymax = max(max(abs(c[1]) for c in o.bound_box) for o in lods)
    xmax = max(max(abs(c[0]) for c in o.bound_box) for o in lods)

    # Ground, sun, sky.
    gm = bpy.data.meshes.new("_ground")
    g = xmax * 40
    gm.from_pydata([(-g, -g, 0.0), (g, -g, 0.0), (g, g, 0.0), (-g, g, 0.0)], [], [(0, 1, 2, 3)])
    add(bpy.data.objects.new("_ground", gm))
    gmat = bpy.data.materials.new("_ground")
    use_nodes(gmat)
    gbsdf = gmat.node_tree.nodes["Principled BSDF"]
    gbsdf.inputs["Base Color"].default_value = (*srgb_to_linear((172, 128, 74)), 1)
    gbsdf.inputs["Roughness"].default_value = 1.0
    gm.materials.append(gmat)
    sun = add(bpy.data.objects.new("_sun", bpy.data.lights.new("_sun", "SUN")))
    sun.data.energy = 3.5
    sun.data.color = (1.0, 0.84, 0.64)
    sun.data.angle = math.radians(1.0)
    sun.rotation_euler = (math.radians(64), 0, math.radians(-40))
    world = bpy.data.worlds.new("_sky")
    use_nodes(world)
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.40, 0.55, 0.82, 1)
    world.node_tree.nodes["Background"].inputs["Strength"].default_value = 0.8
    cam = add(bpy.data.objects.new("_cam", bpy.data.cameras.new("_cam")))
    old_world, old_cam = scene.world, scene.camera
    scene.world, scene.camera = world, cam

    r = scene.render
    r.engine = "CYCLES"
    r.resolution_percentage = 100
    scene.cycles.device = "CPU"
    scene.cycles.samples = 32
    try:
        scene.cycles.use_denoising = True
    except Exception:
        pass
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"

    def shoot(file, res):
        r.resolution_x, r.resolution_y = res
        r.filepath = os.path.abspath(file)
        bpy.ops.render.render(write_still=True)
        print(f"wrote {file}")

    try:
        # 1. Contact sheet: levels in a row, orthographic, slightly from above.
        cursor = 0.0
        for o in lods:
            bx = [c[0] for c in o.bound_box]
            o.location = (cursor - min(bx), 0, 0)
            cursor += (max(bx) - min(bx)) * 1.02
        for o in objs:
            o.hide_render = o not in lods
        total = cursor
        e = math.radians(14)
        lo, hi = -ymax * math.sin(e), zmax * math.cos(e) + ymax * math.sin(e) * 0.2
        up = mathutils.Vector((0, math.sin(e), math.cos(e)))
        view = mathutils.Vector((0, math.cos(e), -math.sin(e)))
        target = mathutils.Vector((total / 2, 0, 0)) + up * ((lo + hi) / 2)
        cam.data.type = "ORTHO"
        cam.data.ortho_scale = total * 1.03
        cam.data.clip_end = total * 50
        cam.location = target - view * total * 5
        cam.rotation_euler = (math.pi / 2 - e, 0, 0)
        height = int(width * (hi - lo) * 1.15 / (total * 1.03)) // 2 * 2
        shoot(path, (width, max(height, 160)))

        # 2. Hero: LOD0 alone, low perspective view.
        for o in objs:
            o.hide_render = o is not lods[0]
        lods[0].location = (0, 0, 0)
        cam.data.type = "PERSP"
        cam.data.lens = 40
        res = (width, width * 9 // 16)
        hfov = 2 * math.atan(18 / cam.data.lens)
        dist = xmax * 0.95 / math.tan(hfov / 2) + ymax * 0.5
        a, e = math.radians(-28), math.radians(7)
        aim = mathutils.Vector((0, 0, zmax * 0.32))
        cam.location = aim + mathutils.Vector((math.sin(a) * math.cos(e), -math.cos(a) * math.cos(e), math.sin(e))) * dist
        cam.rotation_euler = (aim - cam.location).to_track_quat("-Z", "Y").to_euler()
        base, ext = os.path.splitext(path)
        shoot(f"{base}_hero{ext or '.png'}", res)
    finally:
        scene.camera, scene.world = old_cam, old_world
        for o in temp_objs:
            bpy.data.objects.remove(o)
        for d in temp_data:
            if isinstance(d, bpy.types.Mesh):
                bpy.data.meshes.remove(d)
            elif isinstance(d, bpy.types.Light):
                bpy.data.lights.remove(d)
            elif isinstance(d, bpy.types.Camera):
                bpy.data.cameras.remove(d)
        bpy.data.materials.remove(gmat)
        bpy.data.worlds.remove(world)
        for o, loc, hidden in saved:
            o.location, o.hide_render = loc, hidden


# =============================================================================
# Entry points
# =============================================================================

def parse_value(text):
    if "," in text:
        return tuple(float(t) for t in text.split(","))
    try:
        return int(text)
    except ValueError:
        try:
            return float(text)
        except ValueError:
            return text


def main(argv):
    ap = argparse.ArgumentParser(prog="savanna_mountains.py",
                                 description="Generate savanna mountains with LODs and export glTF.")
    ap.add_argument("--preset", default="mesa", help=f"one of {', '.join(PRESETS)}, or 'all'")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--count", type=int, default=1, help="variants per preset (seeds seed..seed+count-1)")
    ap.add_argument("--lods", help="triangle budget per level, e.g. 160000,50000,16000,5000")
    ap.add_argument("--set", action="append", default=[], metavar="KEY=VALUE",
                    help="override a parameter, e.g. --set height=320 --set boulder_size=3,9")
    ap.add_argument("--collision", action="store_true", help="also build a <name>_COL proxy")
    ap.add_argument("--out", help="output .glb/.gltf (single mountain)")
    ap.add_argument("--out-dir", help="output folder; files are named <preset>_<seed>.glb")
    ap.add_argument("--split", action="store_true", help="also write one file per level (<name>_lod0.glb...)")
    ap.add_argument("--blend", help="save the .blend here")
    ap.add_argument("--preview", help="render a PNG of the levels side by side (Cycles, CPU)")
    ap.add_argument("--list", action="store_true", help="print the presets' parameters and exit")
    args = ap.parse_args(argv)

    if args.list:
        for p in PRESETS:
            print(f"--- {p}")
            for k, v in resolve_params(p).items():
                print(f"  {k} = {v}")
        return

    overrides = {}
    for item in args.set:
        k, _, v = item.partition("=")
        if k not in BASE:
            ap.error(f"unknown parameter '{k}' (see --list)")
        overrides[k] = parse_value(v)
    if args.lods:
        overrides["lods"] = tuple(int(t) for t in args.lods.split(","))

    presets = list(PRESETS) if args.preset == "all" else [args.preset]
    for p in presets:
        if p not in PRESETS:
            ap.error(f"unknown preset '{p}'")
    jobs = [(p, args.seed + i) for p in presets for i in range(args.count)]
    if args.out and len(jobs) > 1:
        ap.error("--out names one file; use --out-dir for several mountains")

    # Start from an empty scene.
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o)

    for preset, seed in jobs:
        objs = generate(preset, seed, overrides, collision=args.collision)
        stem = f"{preset}_{seed:02d}"
        out = args.out or (os.path.join(args.out_dir, stem + ".glb") if args.out_dir else None)
        if out:
            export_gltf(objs, out)
            if args.split:
                base, ext = os.path.splitext(out)
                for i, o in enumerate(x for x in objs if "_LOD" in x.name):
                    export_gltf([o], f"{base}_lod{i}{ext}")
        if args.preview:
            pv = args.preview if len(jobs) == 1 else os.path.join(
                os.path.dirname(args.preview) or ".", f"{stem}_{os.path.basename(args.preview)}")
            render_preview(objs, pv)
        if len(jobs) > 1:  # keep the scene small between mountains (unless saving a .blend)
            if not args.blend:
                for o in objs:
                    m = o.data
                    bpy.data.objects.remove(o)
                    bpy.data.meshes.remove(m)
    if args.blend:
        bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(args.blend))


def run_from_ui():
    cfg = UI_CONFIG
    objs = generate(cfg["preset"], cfg["seed"], {"lods": cfg["lods"]} if cfg["lods"] else None,
                    collision=cfg["collision"], side_by_side=cfg["side_by_side"])
    if cfg["out"]:
        export_gltf(objs, bpy.path.abspath(cfg["out"]))


if __name__ == "__main__":
    if "--" in sys.argv:
        main(sys.argv[sys.argv.index("--") + 1:])
    elif bpy.app.background and sys.argv[0].endswith(".py"):
        main(sys.argv[1:])  # `python savanna_mountains.py ...` with the bpy pip module
    else:
        run_from_ui()
