"""Procedural trees and plants for vulkan-engine (pure Python + numpy).

Used three ways:
  - from Blender (tools/foliage/blender_foliage.py: an add-on panel and a
    batch build), which turns the meshes below into editable objects;
  - on its own: `python3 tools/foliage/treegen.py <out dir>` writes .gltf files
    directly (no Blender needed);
  - as a library: `generate("acacia", seed)` -> Plant (LOD meshes per material).

How a tree is made:
  1. Skeleton. An explicit trunk (and, per species, leaders / limbs), then the
     crown filled by space colonisation (Runions et al. 2007): attraction
     points sampled in the species' crown envelope (an umbrella lens for
     acacias, an ellipsoid for fever trees, a dome for jungle trees) pull new
     nodes out of the nearest branches until the crown is filled. That gives
     natural branching that fills the shape without crossing itself.
  2. Radii by the pipe model (a parent's cross-section ~ the sum of its
     children's), scaled so the trunk has the species' thickness.
  3. Bark mesh: each branch is a chain from its fork to its tip; rings along
     it (parallel-transported frames, so no twisting), sides from its radius,
     UVs at a constant texel density, trunk flare / buttresses where asked.
  4. Leaves: alpha-tested cards (sprays of leaves from textures.py) at the
     thin branch ends, oriented per species (flat layers for acacias), with
     normals bent outwards from the crown centre (soft, volumetric shading)
     and vertex-colour ambient occlusion (darker deep in the crown).
  5. LODs: thinner branches dropped and fewer sides, fewer but bigger cards.
     The engine bakes an impostor after the last level.
  6. A convex collision shape around the trunk (*_COL), and a stump model for
     the "chopped" state.

Units: metres, Y up, the model's front +Z (glTF). Leaf atlases are 2x2 cells
(textures.ATLASES); each card's attachment point is the bottom centre of its cell.
"""

import json
import math
import os
import random
import struct
import sys

import numpy as np

# -----------------------------------------------------------------------------
# Small helpers
# -----------------------------------------------------------------------------


def norm(v):
    n = np.linalg.norm(v)
    return v / n if n > 1e-9 else v


def rotate(v, axis, angle):
    """Rodrigues rotation of v around a unit axis."""
    axis = norm(axis)
    c, s = math.cos(angle), math.sin(angle)
    return v * c + np.cross(axis, v) * s + axis * np.dot(axis, v) * (1 - c)


def perpendicular(v):
    a = np.array([0.0, 1.0, 0.0]) if abs(v[1]) < 0.9 else np.array([1.0, 0.0, 0.0])
    return norm(np.cross(v, a))


def lerp(a, b, t):
    return a + (b - a) * t


def face_up(out, rnd, roll=0.5):
    """A card face normal perpendicular to `out`, as close to up as allowed, rolled randomly."""
    up = np.array([0.0, 1.0, 0.0])
    f = up - out * float(np.dot(up, out))
    f = norm(f) if np.linalg.norm(f) > 1e-3 else perpendicular(out)
    return norm(rotate(f, out, rnd.uniform(-roll, roll)))


# -----------------------------------------------------------------------------
# Skeleton
# -----------------------------------------------------------------------------


class Skeleton:
    def __init__(self):
        self.pos = []      # np.array(3)
        self.parent = []   # int (-1 = root)
        self.children = []
        self.radius = []   # set by pipe_radii (or explicitly)
        self.fixed = []    # explicit radius (trunk profiles): not touched by pipe_radii
        self.tag = []      # 'trunk', 'limb', 'branch'

    def add(self, p, parent, tag="branch", radius=0.0, fixed=False):
        i = len(self.pos)
        self.pos.append(np.array(p, dtype=np.float64))
        self.parent.append(parent)
        self.children.append([])
        self.radius.append(radius)
        self.fixed.append(fixed)
        self.tag.append(tag)
        if parent >= 0:
            self.children[parent].append(i)
        return i

    def chain(self, start, direction, length, step, parent, rnd, wobble=0.15, tropism=(0, 0, 0), tag="limb", radius=None):
        """A gently wandering line of nodes; returns their indices (the first continues `start`)."""
        idx = []
        p = np.array(start, dtype=np.float64)
        d = norm(np.array(direction, dtype=np.float64))
        n = max(1, int(round(length / step)))
        prev = parent
        for k in range(n):
            d = norm(d + np.array([rnd.gauss(0, wobble), rnd.gauss(0, wobble * 0.5), rnd.gauss(0, wobble)]) + np.array(tropism))
            p = p + d * (length / n)
            r = None if radius is None else radius(k / max(1, n - 1))
            prev = self.add(p, prev, tag, radius=r or 0.0, fixed=r is not None)
            idx.append(prev)
        return idx

    def tips(self):
        return [i for i in range(len(self.pos)) if not self.children[i]]


def space_colonize(sk, sources, attractors, step, influence, kill, tropism=(0, 0, 0), iterations=120, rnd=None, jitter=0.1,
                   max_children=3):
    """Grows nodes from `sources` (and what they grow) towards the attractors."""
    att = np.array(attractors, dtype=np.float64)
    active = list(sources)
    trop = np.array(tropism, dtype=np.float64)
    for _ in range(iterations):
        if len(att) == 0 or not active:
            break
        nodes = np.array([sk.pos[i] for i in active])
        # Each attractor pulls its nearest node (within the influence radius).
        d2 = ((att[:, None, :] - nodes[None, :, :]) ** 2).sum(-1)
        nearest = d2.argmin(1)
        close = d2[np.arange(len(att)), nearest] < influence * influence
        if not close.any():
            break
        pulls = {}
        for a_i in np.nonzero(close)[0]:
            n_i = nearest[a_i]
            pulls.setdefault(n_i, []).append(norm(att[a_i] - nodes[n_i]))
        grown = []
        for n_i, dirs in pulls.items():
            node = active[n_i]
            if len(sk.children[node]) >= max_children:
                continue
            d = norm(np.mean(dirs, axis=0) + trop + (np.array([rnd.gauss(0, jitter) for _ in range(3)]) if rnd else 0))
            grown.append(sk.add(sk.pos[node] + d * step, node))
        if not grown:
            break
        active += grown
        # Attractors reached are used up.
        newp = np.array([sk.pos[i] for i in grown])
        dk = ((att[:, None, :] - newp[None, :, :]) ** 2).sum(-1).min(1)
        att = att[dk > kill * kill]
    return sk


def pipe_radii(sk, tip, exponent=2.3, root_radius=None):
    """Radius from the tips down (cross-sections add up); fixed radii stay; scaled to root_radius."""
    order = sorted(range(len(sk.pos)), key=lambda i: depth_of(sk, i), reverse=True)
    r = [tip] * len(sk.pos)
    for i in order:
        if sk.children[i]:
            r[i] = sum(r[c] ** exponent for c in sk.children[i]) ** (1.0 / exponent)
    if root_radius:  # rescale keeping the tips: r' = tip + (r - tip) * k
        k = (root_radius - tip) / max(r[0] - tip, 1e-6)
        r = [tip + (x - tip) * k for x in r]
    for i in range(len(sk.pos)):
        if sk.fixed[i]:
            continue
        sk.radius[i] = r[i]
    # A child never thicker than its parent.
    for i in range(len(sk.pos)):
        p = sk.parent[i]
        if p >= 0:
            sk.radius[i] = min(sk.radius[i], sk.radius[p] * 0.98)
    return sk


_depth_cache = {}


def depth_of(sk, i):
    d = 0
    while sk.parent[i] >= 0:
        i = sk.parent[i]
        d += 1
    return d


def tip_distance(sk):
    """Per node: the shortest path length down to a tip of its subtree."""
    order = sorted(range(len(sk.pos)), key=lambda i: depth_of(sk, i), reverse=True)
    d = [0.0] * len(sk.pos)
    for i in order:
        if sk.children[i]:
            d[i] = min(d[c] + float(np.linalg.norm(sk.pos[c] - sk.pos[i])) for c in sk.children[i])
    return d


def smooth(sk, iterations=2, amount=0.5, keep=("trunk",)):
    """Laplacian smoothing of positions along chains (kinks from the colonisation steps)."""
    for _ in range(iterations):
        new = [p.copy() for p in sk.pos]
        for i in range(len(sk.pos)):
            p = sk.parent[i]
            if p < 0 or len(sk.children[i]) != 1 or sk.tag[i] in keep:
                continue
            c = sk.children[i][0]
            new[i] = lerp(sk.pos[i], (sk.pos[p] + sk.pos[c]) * 0.5, amount)
        sk.pos = new
    return sk


def sag(sk, amount):
    """Gravity on thin, long branches: they droop a little towards their ends."""
    for i in range(len(sk.pos)):
        if sk.tag[i] == "trunk":
            continue
        reach = 0.0
        j = i
        while sk.parent[j] >= 0 and sk.tag[j] != "trunk":
            reach += np.linalg.norm(sk.pos[j] - sk.pos[sk.parent[j]])
            j = sk.parent[j]
        sk.pos[i] = sk.pos[i] - np.array([0, amount * reach * reach / max(sk.radius[i] * 60.0, 0.6), 0])
    return sk


def chains(sk):
    """Branches as node chains: each follows its thickest child; the others start new chains (from the fork)."""
    out = []
    starts = [0]
    while starts:
        s = starts.pop()
        chain = [sk.parent[s]] if sk.parent[s] >= 0 else []
        i = s
        while True:
            chain.append(i)
            kids = sorted(sk.children[i], key=lambda c: -sk.radius[c])
            if not kids:
                break
            starts += kids[1:]
            i = kids[0]
        out.append(chain)
    return out


# -----------------------------------------------------------------------------
# Meshes
# -----------------------------------------------------------------------------


class MeshBuf:
    def __init__(self):
        self.p, self.n, self.uv, self.c, self.i = [], [], [], [], []

    def vert(self, p, n, uv, c=(1.0, 1.0, 1.0, 1.0)):
        self.p.append(tuple(float(x) for x in p))
        self.n.append(tuple(float(x) for x in norm(np.array(n, dtype=np.float64))))
        self.uv.append((float(uv[0]), float(uv[1])))
        self.c.append(tuple(float(x) for x in c))
        return len(self.p) - 1

    def tri(self, a, b, c):
        self.i += [a, b, c]

    def quad(self, a, b, c, d):
        self.i += [a, b, c, a, c, d]

    def append(self, other):
        o = len(self.p)
        self.p += other.p
        self.n += other.n
        self.uv += other.uv
        self.c += other.c
        self.i += [x + o for x in other.i]

    def empty(self):
        return not self.i

    @property
    def triangles(self):
        return len(self.i) // 3


def build_bark(sk, buf, min_radius, side_scale, tile, flare=None, max_height=None, cap=False):
    """Rings along every chain whose start radius >= min_radius. tile: metres of bark per texture repeat."""
    for chain in chains(sk):
        if len(chain) < 2:
            continue
        r0 = sk.radius[chain[1] if len(chain) > 1 else chain[0]]
        if r0 < min_radius:
            continue
        pts = [sk.pos[i] for i in chain]
        rad = [sk.radius[i] for i in chain]
        if sk.parent[chain[0]] < 0 and sk.tag[chain[0]] == "trunk":
            pass
        else:
            rad[0] = rad[1]  # (the fork point: start at the branch's own radius, inside the parent)
        if max_height is not None:  # (stumps: cut)
            cut = []
            for k, p in enumerate(pts):
                if p[1] > max_height:
                    if k > 0:
                        t = (max_height - pts[k - 1][1]) / max(p[1] - pts[k - 1][1], 1e-6)
                        cut.append((lerp(pts[k - 1], p, t), lerp(rad[k - 1], rad[k], t)))
                    break
                cut.append((p, rad[k]))
            if len(cut) < 2:
                continue
            pts, rad = [c[0] for c in cut], [c[1] for c in cut]
        sides = int(max(3, min(18, round(side_scale * (5 + r0 * 38)))))
        u_rep = max(1, int(round(2 * math.pi * r0 / tile)))
        # Parallel-transported frames.
        tangents = []
        for k in range(len(pts)):
            a = pts[max(k - 1, 0)]
            b = pts[min(k + 1, len(pts) - 1)]
            tangents.append(norm(b - a))
        normal = perpendicular(tangents[0])
        rings = []
        v = 0.0
        for k, p in enumerate(pts):
            if k > 0:
                axis = np.cross(tangents[k - 1], tangents[k])
                if np.linalg.norm(axis) > 1e-6:
                    ang = math.acos(max(-1.0, min(1.0, float(np.dot(tangents[k - 1], tangents[k])))))
                    normal = rotate(normal, axis, ang)
                v += np.linalg.norm(p - pts[k - 1]) / tile
            binormal = np.cross(tangents[k], normal)
            ring = []
            for s in range(sides + 1):
                a = 2 * math.pi * s / sides
                dirv = normal * math.cos(a) + binormal * math.sin(a)
                r = rad[k]
                if flare:
                    r *= flare(p[1], a)
                ao = 0.75 + 0.25 * min(1.0, p[1] / 2.0) if sk.tag[chain[min(k, len(chain) - 1)]] == "trunk" else 1.0
                ring.append(buf.vert(p + dirv * r, dirv, (s / sides * u_rep, -v), (ao, ao, ao, 1.0)))
            rings.append(ring)
        for k in range(len(rings) - 1):
            for s in range(sides):
                buf.quad(rings[k][s], rings[k][s + 1], rings[k + 1][s + 1], rings[k + 1][s])
        # Close the tip (a small cone) or, for stumps, a flat cut face.
        last = rings[-1]
        tipn = tangents[-1]
        if cap:
            centre = buf.vert(pts[-1], tipn, (0.5, 0.5), (1.35, 1.15, 0.9, 1.0))  # pale cut wood
            for s in range(sides):
                buf.tri(last[s], last[s + 1], centre)
        else:
            tip = buf.vert(pts[-1] + tipn * rad[-1] * 1.5, tipn, (0.5, -v - 0.1))
            for s in range(sides):
                buf.tri(last[s], last[s + 1], tip)


def leaf_card(buf, at, outward, up, width, height, cell, grid, normal_fn, color):
    """A quad whose bottom-centre is at `at`, growing along `outward`; `up` is its face normal."""
    side = norm(np.cross(outward, up)) * (width * 0.5)
    top = outward * height
    u0, v0 = (cell % grid) / grid, (cell // grid) / grid
    du = dv = 1.0 / grid
    corners = [(at - side, (u0, v0 + dv)), (at + side, (u0 + du, v0 + dv)), (at + side + top, (u0 + du, v0)), (at - side + top, (u0, v0))]
    idx = [buf.vert(p, normal_fn(p, up), uv, color) for p, uv in corners]
    buf.quad(*idx)


def crown_normal(centre, squash, blend):
    """Normals pointing out of the crown (an ellipsoid around `centre`) blended with the card's own."""
    def fn(p, face):
        out = norm((p - centre) * np.array([1.0, 1.0 / squash, 1.0]))
        f = np.array(face) if np.dot(face, out) >= 0 else -np.array(face)
        return norm(lerp(f, out, blend))
    return fn


# -----------------------------------------------------------------------------
# Plants
# -----------------------------------------------------------------------------


class Plant:
    """LOD meshes per material ('bark', 'leaves'), a collision hull, a stump."""

    def __init__(self, species, name):
        self.species = species
        self.name = name
        self.lods = []        # [{material: MeshBuf}]
        self.collider = None  # MeshBuf (positions only matter)
        self.stump = None     # {material: MeshBuf}
        self.stump_collider = None


SPECIES = {}


def species(name, bark, leaves, bark_tile=0.9, leaf_rough=0.75, stump=True):
    def deco(fn):
        SPECIES[name] = {"fn": fn, "bark": bark, "leaves": leaves, "tile": bark_tile, "stump": stump}
        return fn
    return deco


def sample_lens(rnd, n, centre, radius, top, bottom, power=0.4):
    """Points in a flat-topped lens (umbrella crowns): r <= radius, bottom..top shaped by (1 - (r/R)^2)."""
    pts = []
    while len(pts) < n:
        x, z = rnd.uniform(-1, 1), rnd.uniform(-1, 1)
        q = x * x + z * z
        if q > 1:
            continue
        f = (1 - q)
        hi = centre[1] + top * f ** power
        lo = centre[1] - bottom * f
        pts.append(np.array([centre[0] + x * radius, rnd.uniform(lo, hi), centre[2] + z * radius]))
    return pts


def sample_ellipsoid(rnd, n, centre, radii, shell=0.0):
    pts = []
    while len(pts) < n:
        v = np.array([rnd.uniform(-1, 1) for _ in range(3)])
        q = float(np.dot(v, v))
        if q > 1 or q < shell * shell:
            continue
        pts.append(np.array(centre) + v * np.array(radii))
    return pts


def foliage_cards(sk, buf, rnd, reach, size, per_node, grid, orient, normal_fn, ao_fn, spread=0.0, skip=0.0):
    """Cards along the last `reach` metres of every twig: orient(node, direction) -> (outward, face normal)."""
    cards = []
    tipd = tip_distance(sk)
    for i in range(len(sk.pos)):
        if tipd[i] > reach or sk.parent[i] < 0 or sk.tag[i] == "trunk":
            continue
        if rnd.random() < skip:
            continue
        p = sk.pos[i]
        d = norm(p - sk.pos[sk.parent[i]])
        for _ in range(per_node if isinstance(per_node, int) else rnd.randint(*per_node)):
            outward, face = orient(p, d, rnd)
            s = size * rnd.uniform(0.8, 1.2)
            at = p + np.array([rnd.uniform(-spread, spread) for _ in range(3)])
            cards.append((at, outward, face, s, rnd.randrange(grid * grid), ao_fn(at), rnd.uniform(0.9, 1.08)))
    return cards


def emit_cards(cards, buf, grid, normal_fn, keep=1.0, grow=1.0, rnd=None, aspect=1.0):
    chosen = cards
    if keep < 1.0:
        # Keep the outer ones (they're what you see from afar).
        chosen = sorted(cards, key=lambda c: -c[5])[: max(1, int(len(cards) * keep))]
    for (at, outward, face, s, cell, ao, tint) in chosen:
        col = (ao * tint, ao * tint, ao * (0.95 + 0.05 * tint), 1.0)
        leaf_card(buf, at - outward * s * grow * 0.15, outward, face, s * grow, s * grow * aspect, cell, grid, normal_fn, col)


def finish_tree(plant, sk, rnd, tile, cards, normal_fn, collider_height, flare=None, card_aspect=1.0, lod_drop=(0.0, 0.03, 0.08),
                lod_keep=(1.0, 0.4, 0.13), lod_sides=(1.0, 0.6, 0.4)):
    for lvl in range(3):
        bark, leaves = MeshBuf(), MeshBuf()
        build_bark(sk, bark, lod_drop[lvl], lod_sides[lvl], tile, flare=flare)
        k = lod_keep[lvl]
        emit_cards(cards, leaves, 2, normal_fn, keep=k, grow=1.0 / math.sqrt(max(k, 1e-3)) * 0.92 if k < 1 else 1.0, aspect=card_aspect)
        plant.lods.append({"bark": bark, "leaves": leaves})
    # Collision: the trunk up to collider_height (its convex hull in the engine).
    col = MeshBuf()
    build_bark(sk, col, 0.08, 0.5, tile, flare=flare, max_height=collider_height)
    plant.collider = col
    # Stump: the trunk cut at 0.6 m.
    stump = MeshBuf()
    build_bark(sk, stump, 0.05, 1.0, tile, flare=flare, max_height=0.6, cap=True)
    plant.stump = {"bark": stump}
    sc = MeshBuf()
    build_bark(sk, sc, 0.08, 0.5, tile, flare=flare, max_height=0.6)
    plant.stump_collider = sc
    return plant


@species("acacia", bark="acacia_bark", leaves="acacia_leaves", bark_tile=0.8)
def acacia(seed):
    """Umbrella thorn (Vachellia tortilis): a short trunk splitting low into a few
    leaning leaders that fan out under a wide, flat-topped crown of layered foliage."""
    rnd = random.Random(seed)
    sk = Skeleton()
    trunk_h = rnd.uniform(1.0, 2.0)
    base_r = rnd.uniform(0.17, 0.25)
    lean = np.array([rnd.uniform(-0.15, 0.15), 1.0, rnd.uniform(-0.15, 0.15)])
    root = sk.add((0, 0, 0), -1, "trunk", radius=base_r * 1.25, fixed=True)
    trunk = sk.chain((0, 0, 0), lean, trunk_h, 0.35, root, rnd, wobble=0.05, tag="trunk")
    crown_h = trunk_h + rnd.uniform(3.2, 4.6)
    radius = rnd.uniform(4.8, 6.8)
    # Leaders: 2-4, fanning out and up to the crown's underside.
    n = rnd.choice((2, 3, 3, 4))
    az0 = rnd.uniform(0, 2 * math.pi)
    sources = []
    for k in range(n):
        az = az0 + 2 * math.pi * k / n + rnd.uniform(-0.4, 0.4)
        out = rnd.uniform(0.45, 0.8)  # how far it leans out
        d = np.array([math.cos(az) * out, 1.0, math.sin(az) * out])
        length = (crown_h - trunk_h - 0.8) / norm(d)[1] * rnd.uniform(0.85, 1.05)
        leader = sk.chain(sk.pos[trunk[-1]], d, length, 0.4, trunk[-1], rnd, wobble=0.12, tag="limb")
        sources += leader[len(leader) // 2:]
    # The crown: a flat-topped lens, wider than tall.
    centre = np.array([0.0, crown_h, 0.0])
    att = sample_lens(rnd, 1600, centre, radius, top=0.8, bottom=1.2, power=0.25)
    space_colonize(sk, sources, att, step=0.32, influence=2.0, kill=0.55, tropism=(0, 0.05, 0), rnd=rnd, jitter=0.15)
    pipe_radii(sk, 0.012, 2.4, root_radius=base_r)
    smooth(sk, 2, 0.5)
    sag(sk, 0.006)
    # Flat layers of leaves: cards lie nearly horizontal, pointing outwards.
    normal_fn = crown_normal(centre + np.array([0, -0.8, 0]), 0.45, 0.7)

    def orient(p, d, r):
        out = np.array([d[0], 0.0, d[2]])
        out = norm(out if np.linalg.norm(out) > 0.1 else np.array([r.uniform(-1, 1), 0, r.uniform(-1, 1)]))
        out = rotate(out, np.array([0, 1.0, 0]), r.uniform(-0.8, 0.8))
        tilt = r.uniform(-0.25, 0.35)  # mostly flat
        out = norm(out + np.array([0, tilt, 0]))
        face = face_up(out, r, 0.35)
        return out, face

    def ao(p):
        rr = math.hypot(p[0], p[2]) / radius
        h = (p[1] - (crown_h - 1.4)) / 2.1
        return 0.45 + 0.55 * min(1.0, max(0.0, 0.55 * rr + 0.55 * h))

    cards = foliage_cards(sk, None, rnd, 0.9, 1.25, (1, 2), 2, orient, normal_fn, ao, spread=0.2)
    plant = Plant("acacia", f"Acacia{seed}")
    return finish_tree(plant, sk, rnd, 0.8, cards, normal_fn, collider_height=trunk_h + 1.6)  # (the trunk and the leaders' bases)


@species("fever_tree", bark="fever_bark", leaves="fever_leaves", bark_tile=1.2)
def fever_tree(seed):
    """Fever tree (Vachellia xanthophloea): a tall, straight yellow trunk, ascending
    limbs and an open, airy crown."""
    rnd = random.Random(seed)
    sk = Skeleton()
    h = rnd.uniform(10.0, 14.0)
    base_r = rnd.uniform(0.24, 0.34)
    root = sk.add((0, 0, 0), -1, "trunk", radius=base_r * 1.2, fixed=True)
    fork = h * rnd.uniform(0.32, 0.42)
    trunk = sk.chain((0, 0, 0), (rnd.uniform(-0.1, 0.1), 1, rnd.uniform(-0.1, 0.1)), fork, 0.5, root, rnd, wobble=0.04, tag="trunk")
    # 2-4 steep, ascending limbs.
    sources = []
    n = rnd.choice((2, 3, 3, 4))
    az0 = rnd.uniform(0, 2 * math.pi)
    for k in range(n):
        az = az0 + 2 * math.pi * k / n + rnd.uniform(-0.4, 0.4)
        out = rnd.uniform(0.25, 0.5)
        limb = sk.chain(sk.pos[trunk[-1]], (math.cos(az) * out, 1.0, math.sin(az) * out), h * rnd.uniform(0.3, 0.4), 0.5, trunk[-1], rnd,
                        wobble=0.1, tag="limb")
        sources += limb[len(limb) // 3:]
    centre = np.array([0.0, h * 0.74, 0.0])
    att = sample_ellipsoid(rnd, 900, centre, (h * 0.42, h * 0.27, h * 0.42), shell=0.35)
    space_colonize(sk, sources, att, step=0.45, influence=2.8, kill=0.75, tropism=(0, 0.18, 0), rnd=rnd, jitter=0.15)
    pipe_radii(sk, 0.012, 2.3, root_radius=base_r)
    smooth(sk, 2, 0.5)
    normal_fn = crown_normal(centre, 0.8, 0.65)

    def orient(p, d, r):
        out = norm(d + np.array([r.uniform(-0.6, 0.6), r.uniform(-0.2, 0.5), r.uniform(-0.6, 0.6)]))
        face = norm(np.cross(out, perpendicular(out)))
        if face[1] < 0:
            face = -face
        return out, face

    def ao(p):
        return 0.55 + 0.45 * min(1.0, np.linalg.norm((p - centre) / np.array([h * 0.42, h * 0.3, h * 0.42])))

    cards = foliage_cards(sk, None, rnd, 1.1, 1.05, 1, 2, orient, normal_fn, ao, spread=0.12, skip=0.3)
    plant = Plant("fever_tree", f"FeverTree{seed}")
    return finish_tree(plant, sk, rnd, 1.2, cards, normal_fn, collider_height=fork)


@species("baobab", bark="baobab_bark", leaves="baobab_leaves", bark_tile=2.6)
def baobab(seed):
    """Baobab: a massive bottle-shaped trunk with stubby, root-like limbs and sparse leaf clusters."""
    rnd = random.Random(seed)
    sk = Skeleton()
    h = rnd.uniform(5.5, 7.5)
    r = rnd.uniform(1.3, 1.9)

    def profile(t):  # bottle: wide base, belly, narrowing top
        return r * (1.12 - 0.12 * t + 0.06 * math.sin(math.pi * t) - 0.38 * t ** 3) + r * 0.25 * max(0.0, 0.15 - t) / 0.15

    root = sk.add((0, 0, 0), -1, "trunk", radius=profile(0.0), fixed=True)
    trunk = sk.chain((0, 0, 0), (0, 1, 0), h, 0.45, root, rnd, wobble=0.02, tag="trunk", radius=profile)
    top = sk.pos[trunk[-1]]
    tip_nodes = []
    n = rnd.randint(6, 9)

    def limb(start, parent, d, length, r0, depth):
        nodes = sk.chain(start, d, length, 0.35, parent, rnd, wobble=0.18, tag="limb", radius=lambda t: r0 * (1 - 0.6 * t))
        if depth < 2:
            for _ in range(rnd.randint(1, 3)):
                nd = norm(d + np.array([rnd.uniform(-0.8, 0.8), rnd.uniform(0.0, 0.6), rnd.uniform(-0.8, 0.8)]))
                limb(sk.pos[nodes[-1]], nodes[-1], nd, length * rnd.uniform(0.45, 0.7), r0 * 0.38, depth + 1)
        else:
            tip_nodes.append(nodes[-1])

    for k in range(n):
        az = 2 * math.pi * k / n + rnd.uniform(-0.3, 0.3)
        out = rnd.uniform(1.0, 2.0)
        d = np.array([math.cos(az) * out, 1.0, math.sin(az) * out])
        start = top + np.array([math.cos(az), 0, math.sin(az)]) * profile(1.0) * 0.4 - np.array([0, 0.3, 0])
        limb(start, trunk[-2], d, rnd.uniform(2.6, 4.2), profile(1.0) * rnd.uniform(0.3, 0.42), 0)
    centre = top + np.array([0, 2.0, 0])
    normal_fn = crown_normal(centre, 0.6, 0.6)

    def orient(p, d, rr):
        out = norm(d + np.array([rr.uniform(-0.5, 0.5), rr.uniform(0, 0.4), rr.uniform(-0.5, 0.5)]))
        face = norm(np.cross(out, perpendicular(out)))
        return out, (face if face[1] >= 0 else -face)

    cards = []
    for i in tip_nodes:
        for _ in range(rnd.randint(1, 3)):
            o, f = orient(sk.pos[i], norm(sk.pos[i] - sk.pos[sk.parent[i]]), rnd)
            cards.append((sk.pos[i], o, f, rnd.uniform(0.9, 1.3), rnd.randrange(4), 0.85, rnd.uniform(0.9, 1.08)))
    plant = Plant("baobab", f"Baobab{seed}")
    return finish_tree(plant, sk, rnd, 2.6, cards, normal_fn, collider_height=h, lod_drop=(0.0, 0.08, 0.2), lod_keep=(1.0, 0.6, 0.3))


@species("jungle_tree", bark="jungle_bark", leaves="jungle_leaves", bark_tile=2.2)
def jungle_tree(seed):
    """A rainforest canopy tree: a tall straight trunk with buttress roots and a broad dome of big leaves."""
    rnd = random.Random(seed)
    sk = Skeleton()
    h = rnd.uniform(14.0, 20.0)
    base_r = rnd.uniform(0.4, 0.6)
    root = sk.add((0, 0, 0), -1, "trunk", radius=base_r, fixed=True)
    trunk = sk.chain((0, 0, 0), (rnd.uniform(-0.05, 0.05), 1, rnd.uniform(-0.05, 0.05)), h, 0.8, root, rnd, wobble=0.03, tag="trunk",
                     radius=lambda t: base_r * (1.0 - 0.45 * t))
    n_but = rnd.randint(4, 6)
    phase = rnd.uniform(0, 2 * math.pi)

    def flare(y, a):  # buttresses: thin fins at the base
        if y > 3.0:
            return 1.0
        fin = max(0.0, math.cos((a + phase) * n_but)) ** 6
        k = (1 - y / 3.0) ** 2
        return 1.0 + k * (0.6 + 2.6 * fin)

    centre = np.array([0.0, h + 2.5, 0.0])
    sources = trunk[-2:]
    for k in range(rnd.randint(3, 5)):  # big limbs forking up and out into the canopy
        az = rnd.uniform(0, 2 * math.pi)
        start = trunk[int(len(trunk) * rnd.uniform(0.72, 0.92))]
        out = rnd.uniform(0.7, 1.3)
        limb = sk.chain(sk.pos[start], (math.cos(az) * out, 1.0, math.sin(az) * out), rnd.uniform(3.5, 6.0), 0.6, start, rnd, wobble=0.1,
                        tag="limb")
        sources += limb[len(limb) // 2:]
    att = sample_lens(rnd, 1300, centre, rnd.uniform(7.0, 9.5), top=3.5, bottom=2.2, power=0.6)
    space_colonize(sk, sources, att, step=0.5, influence=3.0, kill=0.8, tropism=(0, 0.12, 0), rnd=rnd, jitter=0.12)
    pipe_radii(sk, 0.02, 2.3, root_radius=None)
    for i in range(len(sk.pos)):  # (the trunk keeps its explicit taper)
        if sk.tag[i] != "trunk":
            sk.radius[i] = min(sk.radius[i], 0.3)
    smooth(sk, 2, 0.5)
    normal_fn = crown_normal(centre, 0.55, 0.7)

    def orient(p, d, r):
        out = norm(np.array([d[0], 0, d[2]]) + np.array([r.uniform(-0.5, 0.5), r.uniform(0.0, 0.6), r.uniform(-0.5, 0.5)]))
        face = norm(np.cross(np.cross(out, np.array([0, 1.0, 0])), out))
        if np.linalg.norm(face) < 0.1 or face[1] < 0:
            face = np.array([0, 1.0, 0])
        return out, face

    def ao(p):
        return 0.5 + 0.5 * min(1.0, np.linalg.norm((p - centre) / np.array([7.0, 3.0, 7.0])))

    cards = foliage_cards(sk, None, rnd, 1.4, 1.6, (1, 2), 2, orient, normal_fn, ao, spread=0.25)
    plant = Plant("jungle_tree", f"JungleTree{seed}")
    return finish_tree(plant, sk, rnd, 2.2, cards, normal_fn, collider_height=4.0, flare=flare, lod_drop=(0.0, 0.05, 0.12))


@species("bush", bark="acacia_bark", leaves="bush_leaves", bark_tile=0.6, stump=False)
def bush(seed):
    """A savanna shrub: several stems from the ground into a dense, rounded crown."""
    rnd = random.Random(seed)
    sk = Skeleton()
    h = rnd.uniform(1.4, 2.2)
    root = sk.add((0, 0, 0), -1, "trunk", radius=0.06, fixed=True)
    sources = []
    for k in range(rnd.randint(4, 7)):
        az = rnd.uniform(0, 2 * math.pi)
        d = np.array([math.cos(az) * 0.6, 1.0, math.sin(az) * 0.6])
        sources += sk.chain((0, 0, 0), d, h * 0.45, 0.2, root, rnd, wobble=0.15, tag="limb")
    centre = np.array([0.0, h * 0.6, 0.0])
    att = sample_ellipsoid(rnd, 500, centre, (h * 0.75, h * 0.45, h * 0.75))
    space_colonize(sk, sources, att, step=0.18, influence=0.9, kill=0.25, tropism=(0, 0.1, 0), rnd=rnd, jitter=0.2)
    pipe_radii(sk, 0.006, 2.3, root_radius=0.06)
    normal_fn = crown_normal(centre, 0.7, 0.75)

    def orient(p, d, r):
        out = norm(d + np.array([r.uniform(-0.8, 0.8), r.uniform(-0.2, 0.6), r.uniform(-0.8, 0.8)]))
        face = norm(np.cross(out, perpendicular(out)))
        return out, (face if face[1] >= 0 else -face)

    def ao(p):
        return 0.5 + 0.5 * min(1.0, np.linalg.norm((p - centre) / np.array([h * 0.75, h * 0.45, h * 0.75])))

    cards = foliage_cards(sk, None, rnd, 0.5, 0.7, 1, 2, orient, normal_fn, ao, spread=0.08, skip=0.25)
    plant = Plant("bush", f"Bush{seed}")
    finish_tree(plant, sk, rnd, 0.6, cards, normal_fn, collider_height=0.6, lod_drop=(0.0, 0.015, 0.03), lod_keep=(1.0, 0.45, 0.18))
    plant.collider = None
    plant.stump = None
    return plant


# --- Fronds: palms and ferns ---------------------------------------------------------


def frond(buf, base, direction, length, width, droop, cell, grid, fold, segments, normal_fn, color, twist=0.0):
    """An arched strip carrying a frond texture (base at the cell's bottom), folded along its rachis."""
    d = norm(direction)
    side = norm(np.cross(d, np.array([0, 1.0, 0]))) if abs(d[1]) < 0.98 else np.array([1.0, 0, 0])
    side = rotate(side, d, twist)
    up = norm(np.cross(side, d))
    u0, v0 = (cell % grid) / grid, (cell // grid) / grid
    du = dv = 1.0 / grid
    rows = []
    for k in range(segments + 1):
        t = k / segments
        # Arch: rises along the direction, then droops (gravity grows with t^2).
        p = base + d * length * t - np.array([0, droop * length * t * t, 0])
        tang = norm(d - np.array([0, 2 * droop * t, 0]))
        nup = norm(np.cross(side, tang))
        w = width * 0.5
        ring = []
        for s, (sx, lift) in enumerate(((-1.0, fold), (0.0, 0.0), (1.0, fold))):
            q = p + side * (sx * w) + nup * (lift * w)
            ring.append(buf.vert(q, normal_fn(q, nup), (u0 + du * (0.5 + 0.5 * sx * 0.98), v0 + dv * (1.0 - t * 0.99)), color))
        rows.append(ring)
    for k in range(segments):
        for s in range(2):
            buf.quad(rows[k][s], rows[k][s + 1], rows[k + 1][s + 1], rows[k + 1][s])


@species("palm", bark="palm_bark", leaves="palm_frond", bark_tile=1.0)
def palm(seed):
    """A palm: a slender, curving ringed trunk and a crown of arching fronds."""
    rnd = random.Random(seed)
    sk = Skeleton()
    h = rnd.uniform(7.0, 11.0)
    r = rnd.uniform(0.17, 0.22)
    bend = rnd.uniform(0.5, 2.0)
    az = rnd.uniform(0, 2 * math.pi)
    root = sk.add((0, 0, 0), -1, "trunk", radius=r * 1.35, fixed=True)
    prev = root
    n = 14
    for k in range(1, n + 1):
        t = k / n
        p = np.array([math.cos(az) * bend * t * t, h * t, math.sin(az) * bend * t * t])
        prev = sk.add(p, prev, "trunk", radius=r * (1.1 - 0.25 * t), fixed=True)
    top = sk.pos[prev]
    centre = top + np.array([0, 0.5, 0])
    normal_fn = crown_normal(centre, 0.7, 0.55)
    fronds = []
    m = rnd.randint(12, 16)
    for k in range(m):
        a = 2 * math.pi * k / m + rnd.uniform(-0.2, 0.2)
        elev = lerp(0.9, -0.25, (k % 4) / 3.0) + rnd.uniform(-0.15, 0.15)  # young (up) to old (drooping)
        d = np.array([math.cos(a) * math.cos(elev), math.sin(elev) + 0.15, math.sin(a) * math.cos(elev)])
        fronds.append((d, rnd.uniform(3.2, 4.6), rnd.uniform(0.25, 0.45), rnd.randrange(4), rnd.uniform(-0.3, 0.3), rnd.uniform(0.85, 1.05)))
    plant = Plant("palm", f"Palm{seed}")
    for lvl, (segs, keep, sides) in enumerate(((8, 1.0, 1.0), (4, 1.0, 0.6), (2, 0.6, 0.4))):
        bark, leaves = MeshBuf(), MeshBuf()
        build_bark(sk, bark, 0.0, sides, 1.0)
        for j, (d, L, droop, cell, tw, tint) in enumerate(fronds):
            if j % int(round(1 / keep)) if keep < 1 else False:
                continue
            frond(leaves, top, d, L, 1.5 * (1.2 if keep < 1 else 1.0), droop, cell, 2, 0.25, segs, normal_fn, (tint, tint, tint, 1.0), tw)
        plant.lods.append({"bark": bark, "leaves": leaves})
    col = MeshBuf()
    build_bark(sk, col, 0.0, 0.5, 1.0, max_height=h)
    plant.collider = col
    stump = MeshBuf()
    build_bark(sk, stump, 0.0, 1.0, 1.0, max_height=0.6, cap=True)
    plant.stump = {"bark": stump}
    sc = MeshBuf()
    build_bark(sk, sc, 0.0, 0.5, 1.0, max_height=0.6)
    plant.stump_collider = sc
    return plant


@species("fern", bark="jungle_bark", leaves="fern_frond", stump=False)
def fern(seed):
    """Jungle undergrowth: a rosette of arching fern fronds."""
    rnd = random.Random(seed)
    plant = Plant("fern", f"Fern{seed}")
    m = rnd.randint(10, 15)
    centre = np.array([0.0, 0.4, 0.0])
    normal_fn = crown_normal(centre, 0.6, 0.5)
    fr = []
    for k in range(m):
        a = 2 * math.pi * k / m + rnd.uniform(-0.25, 0.25)
        elev = rnd.uniform(0.6, 1.1)
        d = np.array([math.cos(a) * math.cos(elev), math.sin(elev), math.sin(a) * math.cos(elev)])
        fr.append((d, rnd.uniform(0.7, 1.2), rnd.uniform(0.6, 1.0), rnd.randrange(4), rnd.uniform(0.85, 1.05)))
    for lvl, (segs, keep) in enumerate(((6, 1.0), (3, 1.0), (2, 0.6))):
        leaves = MeshBuf()
        for j, (d, L, droop, cell, tint) in enumerate(fr):
            if keep < 1 and j % 2:
                continue
            frond(leaves, np.array([0.0, 0.02, 0.0]), d, L, 0.45 * (1.25 if keep < 1 else 1.0), droop, cell, 2, 0.2, segs, normal_fn,
                  (tint, tint, tint, 1.0))
        plant.lods.append({"leaves": leaves})
    return plant


def generate(name, seed):
    return SPECIES[name]["fn"](seed)


# -----------------------------------------------------------------------------
# glTF writer (no Blender needed)
# -----------------------------------------------------------------------------


# Leaf translucency (KHR_materials_diffuse_transmission): how much light
# passes through, and its tint (times the leaf colour). Thin leaves more,
# small leathery acacia leaflets less.
LEAF_TRANSLUCENCY = {
    "acacia_leaves": (0.45, (1.0, 1.0, 0.7)),
    "fever_leaves": (0.5, (1.0, 1.0, 0.7)),
    "baobab_leaves": (0.6, (1.0, 1.0, 0.75)),
    "bush_leaves": (0.45, (1.0, 1.0, 0.75)),
    "jungle_leaves": (0.7, (1.0, 1.05, 0.7)),
    "palm_frond": (0.6, (1.0, 1.0, 0.7)),
    "fern_frond": (0.75, (1.0, 1.05, 0.7)),
}


class Gltf:
    def __init__(self):
        self.bin = bytearray()
        self.views, self.accessors, self.meshes, self.nodes = [], [], [], []
        self.materials, self.textures, self.images = [], [], []
        self.mat_index = {}
        self.extensions = set()

    def _view(self, data, target):
        while len(self.bin) % 4:
            self.bin.append(0)
        off = len(self.bin)
        self.bin += data
        v = {"buffer": 0, "byteOffset": off, "byteLength": len(data)}
        if target:
            v["target"] = target
        self.views.append(v)
        return len(self.views) - 1

    def _acc(self, values, comps, minmax=False):
        flat = [x for v in values for x in v]
        view = self._view(struct.pack("<%df" % len(flat), *flat), 34962)
        a = {"bufferView": view, "componentType": 5126, "count": len(values), "type": {1: "SCALAR", 2: "VEC2", 3: "VEC3", 4: "VEC4"}[comps]}
        if minmax:
            arr = np.array(values)
            a["min"], a["max"] = arr.min(0).tolist(), arr.max(0).tolist()
        self.accessors.append(a)
        return len(self.accessors) - 1

    def _indices(self, idx):
        big = max(idx) > 65535
        view = self._view(struct.pack("<%d%s" % (len(idx), "I" if big else "H"), *idx), 34963)
        self.accessors.append({"bufferView": view, "componentType": 5125 if big else 5123, "count": len(idx), "type": "SCALAR"})
        return len(self.accessors) - 1

    def _texture(self, uri):
        for i, im in enumerate(self.images):
            if im["uri"] == uri:
                return i
        self.images.append({"uri": uri})
        self.textures.append({"source": len(self.images) - 1, "sampler": 0})
        return len(self.textures) - 1

    def material(self, key, tex_dir, name):
        if key in self.mat_index:
            return self.mat_index[key]
        if key == "bark":
            m = {"name": name, "pbrMetallicRoughness": {"baseColorTexture": {"index": self._texture(f"{tex_dir}/{name}_albedo.png")},
                                                         "metallicRoughnessTexture": {"index": self._texture(f"{tex_dir}/{name}_mr.png")},
                                                         "metallicFactor": 0.0, "roughnessFactor": 1.0},
                 "normalTexture": {"index": self._texture(f"{tex_dir}/{name}_normal.png"), "scale": 1.0}}
        else:
            m = {"name": name, "pbrMetallicRoughness": {"baseColorTexture": {"index": self._texture(f"{tex_dir}/{name}.png")},
                                                         "metallicRoughnessTexture": {"index": self._texture(f"{tex_dir}/{name}_mr.png")},
                                                         "metallicFactor": 0.0, "roughnessFactor": 1.0},
                 "normalTexture": {"index": self._texture(f"{tex_dir}/{name}_normal.png"), "scale": 1.0},
                 "alphaMode": "MASK", "alphaCutoff": 0.45, "doubleSided": True}
            strength, tint = LEAF_TRANSLUCENCY.get(name, (0.5, (1.0, 1.0, 0.8)))
            m["extensions"] = {"KHR_materials_diffuse_transmission": {"diffuseTransmissionFactor": strength,
                                                                       "diffuseTransmissionColorFactor": list(tint)}}
            self.extensions.add("KHR_materials_diffuse_transmission")
        self.materials.append(m)
        self.mat_index[key] = len(self.materials) - 1
        return self.mat_index[key]

    def mesh(self, name, parts, materials):
        prims = []
        for key, buf in parts.items():
            if buf is None or buf.empty():
                continue
            prim = {"attributes": {"POSITION": self._acc(buf.p, 3, True), "NORMAL": self._acc(buf.n, 3), "TEXCOORD_0": self._acc(buf.uv, 2),
                                   "COLOR_0": self._acc(buf.c, 4)},
                    "indices": self._indices(buf.i)}
            if materials is not None:
                prim["material"] = materials[key]
            prims.append(prim)
        if not prims:
            return None
        self.meshes.append({"name": name, "primitives": prims})
        return len(self.meshes) - 1

    def node(self, name, mesh, translation):
        n = {"name": name, "translation": [float(x) for x in translation]}
        if mesh is not None:
            n["mesh"] = mesh
        self.nodes.append(n)
        return len(self.nodes) - 1

    def write(self, path):
        base = os.path.splitext(os.path.basename(path))[0]
        doc = {"asset": {"version": "2.0", "generator": "vulkan-engine tools/foliage/treegen.py"},
               "scene": 0, "scenes": [{"nodes": list(range(len(self.nodes)))}], "nodes": self.nodes, "meshes": self.meshes,
               "materials": self.materials, "textures": self.textures, "images": self.images,
               "samplers": [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}],
               "buffers": [{"uri": base + ".bin", "byteLength": len(self.bin)}], "bufferViews": self.views, "accessors": self.accessors}
        if self.extensions:
            doc["extensionsUsed"] = sorted(self.extensions)
        with open(os.path.join(os.path.dirname(path), base + ".bin"), "wb") as f:
            f.write(self.bin)
        with open(path, "w") as f:
            json.dump(doc, f, indent=1)


def write_species(name, seeds, out_dir, tex_dir="../../textures/foliage", spacing=None):
    """<out_dir>/<name>.gltf (variants side by side, LOD0..2 + COL) and <name>_stump.gltf."""
    spec = SPECIES[name]
    plants = [generate(name, s) for s in seeds]
    g = Gltf()
    mats = {"bark": g.material("bark", tex_dir, spec["bark"]), "leaves": g.material("leaves", tex_dir, spec["leaves"])}
    stats = []
    for k, p in enumerate(plants):
        x = (spacing or 15.0) * k
        for lvl, parts in enumerate(p.lods):
            g.node(f"{p.name}_LOD{lvl}", g.mesh(f"{p.name}_LOD{lvl}", parts, mats), (x, 0, 0))
        if p.collider is not None and not p.collider.empty():
            g.node(f"{p.name}_COL", g.mesh(f"{p.name}_COL", {"bark": p.collider}, None), (x, 0, 0))
        stats.append([sum(b.triangles for b in parts.values()) for parts in p.lods])
    os.makedirs(out_dir, exist_ok=True)
    g.write(os.path.join(out_dir, name + ".gltf"))
    if spec["stump"] and all(p.stump for p in plants):
        s = Gltf()
        smats = {"bark": s.material("bark", tex_dir, spec["bark"])}
        for k, p in enumerate(plants):
            x = (spacing or 15.0) * k
            s.node(f"{p.name}Stump_LOD0", s.mesh(f"{p.name}Stump_LOD0", p.stump, smats), (x, 0, 0))
            if p.stump_collider is not None and not p.stump_collider.empty():
                s.node(f"{p.name}Stump_COL", s.mesh(f"{p.name}Stump_COL", {"bark": p.stump_collider}, None), (x, 0, 0))
        s.write(os.path.join(out_dir, name + "_stump.gltf"))
    return stats


DEFAULT_SEEDS = {"acacia": (1, 2, 3, 4), "fever_tree": (1, 2, 3), "baobab": (1, 2), "jungle_tree": (1, 2, 3), "bush": (1, 2, 3, 4),
                 "palm": (1, 2, 3), "fern": (1, 2, 3)}

if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "sandbox/assets/models/foliage"
    only = sys.argv[2:] or list(DEFAULT_SEEDS)
    for name in only:
        stats = write_species(name, DEFAULT_SEEDS[name], out)
        print(f"{name}: triangles per LOD per variant {stats}")
