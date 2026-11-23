"""
VE Procedural Props - Blender add-on
====================================

Rebuilds, inside Blender, the low-poly props the sandbox generates in code so
they can be restyled by hand and exported back as .glb:

  sandbox/src/Vegetation.cpp   acacia, baobab, fever tree, bush, dry bush, rock,
                               boulder, palm, jungle tree, undergrowth, quiver
                               tree, acacia stump, berry bush (+ picked)
  sandbox/src/Inventory.cpp    item meshes: wood, berries, stone, stick, sunstone
  sandbox/src/Props.cpp        campfire, torch, sunstone pickup

The geometry is a line-by-line port: the same primitives (engine/src/ve/render/
Primitives.cpp), the same deterministic Rng / value noise (ve/world/Noise.hpp)
and the same seeds the game registers (acacia 100..104, baobab 200..201, ...),
so variant N here is variant N in the game.

One catch: C++ leaves the evaluation order of function arguments unspecified,
and several calls draw more than one random number in their arguments
(e.g. m.append(blob(rng), trs(..rng..), vary(rng))). GCC and MSVC evaluate them
right-to-left, Clang left-to-right. The "Argument order" option picks which
compiler's shapes to reproduce (default: right-to-left, the GCC/MinGW build).

Coordinates: the engine is Y-up, Blender is Z-up; vertices are converted the
same way Blender's glTF importer does, so exporting as glTF (+Y up, the
default) puts them back in engine space with the base on y = 0.

Each mesh gets one material slot per part (VE_Bark, VE_Leaves, VE_Rock, ...)
and a "Col" colour attribute holding the engine's vertex tint, so restyling
can be done per material or by painting the colours.

UVs ("UVs" option): "Tiling" (default) gives real-world-scale UVs - cylinders
(trunks, branches, logs) wrap around with u = arc length and v = height, caps
and everything else (leaf blobs, rocks, berries) are box-projected per face -
so one tileable bark / leaf / rock texture appears at the same size on every
prop ("Texture size" = metres per repeat). "Unwrapped" runs Smart UV Project
for a non-overlapping 0-1 layout to hand-paint or bake into. The UV map
exports as glTF TEXCOORD_0.

Install: Edit > Preferences > Add-ons > Install from Disk... > pick this file.
Use:     3D Viewport > N sidebar > "VE Props" tab, or Add > Mesh > VE Prop.

The geometry core is plain Python (no bpy), so `python ve_procedural_props.py`
prints the triangle count of every variant as a quick sanity check.
"""

bl_info = {
    "name": "VE Procedural Props",
    "author": "vulkan-engine",
    "version": (1, 0, 0),
    "blender": (3, 6, 0),
    "location": "View3D > Sidebar > VE Props, Add > Mesh > VE Prop",
    "description": "Generates the sandbox's procedural trees, bushes, rocks, items and props for hand styling",
    "category": "Add Mesh",
}

import math

try:
    import bpy
    from bpy.props import BoolProperty, EnumProperty, FloatProperty, IntProperty
except ImportError:  # running the self-test outside Blender
    bpy = None

TAU = math.tau
HALF_PI = math.pi * 0.5
M32 = 0xFFFFFFFF

# =============================================================================
# Argument evaluation order (see the module docstring)
# =============================================================================

_RIGHT_TO_LEFT = True


def _ev(x):
    return x() if callable(x) else x


def args(*items):
    """Evaluates the arguments of one C++ call in the selected compiler's
    order and returns them in declaration order. Items that draw random
    numbers are passed as lambdas; plain values are passed as-is."""
    out = [None] * len(items)
    order = range(len(items) - 1, -1, -1) if _RIGHT_TO_LEFT else range(len(items))
    for i in order:
        out[i] = _ev(items[i])
    return out


# =============================================================================
# Small vector / matrix helpers (glm semantics: column vectors, m * v)
# =============================================================================


def v_add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def v_mul(a, s):
    return (a[0] * s, a[1] * s, a[2] * s)


def v_norm(a):
    l = math.sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2])
    return (a[0] / l, a[1] / l, a[2] / l)


def vec3(*items):
    """glm::vec3(a, b, c) with parentheses: a function call, so argument order applies."""
    return tuple(args(*items))


def m_identity():
    return [[1.0 if i == j else 0.0 for j in range(4)] for i in range(4)]


def m_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def m_translate(m, t):
    r = m_identity()
    r[0][3], r[1][3], r[2][3] = t
    return m_mul(m, r)


def m_rotate(m, angle, axis):
    x, y, z = v_norm(axis)
    c, s = math.cos(angle), math.sin(angle)
    k = 1.0 - c
    r = [
        [c + x * x * k, x * y * k - z * s, x * z * k + y * s, 0.0],
        [y * x * k + z * s, c + y * y * k, y * z * k - x * s, 0.0],
        [z * x * k - y * s, z * y * k + x * s, c + z * z * k, 0.0],
        [0.0, 0.0, 0.0, 1.0],
    ]
    return m_mul(m, r)


def m_scale(m, s):
    r = m_identity()
    r[0][0], r[1][1], r[2][2] = s
    return m_mul(m, r)


def m_point(m, p):
    return tuple(m[i][0] * p[0] + m[i][1] * p[1] + m[i][2] * p[2] + m[i][3] for i in range(3))


def trs(t, yaw, tilt, s=(1.0, 1.0, 1.0)):
    m = m_translate(m_identity(), t)
    m = m_rotate(m, yaw, (0, 1, 0))
    m = m_rotate(m, tilt, (1, 0, 0))
    return m_scale(m, s)


def TRS(*items):
    """trs(...) whose arguments may draw random numbers."""
    return trs(*args(*items))


# =============================================================================
# ve::noise (Noise.hpp) - bit exact integer hashing
# =============================================================================


def hash2(x, y, seed=0):
    h = ((x & M32) * 374761393 + (y & M32) * 668265263 + seed * 2246822519) & M32
    h = ((h ^ (h >> 13)) * 1274126177) & M32
    h ^= h >> 16
    return (h & 0xFFFFFF) / 16777216.0


def value_noise(px, py, seed=0):
    ix, iy = math.floor(px), math.floor(py)
    fx, fy = px - ix, py - iy
    fx = fx * fx * (3.0 - 2.0 * fx)
    fy = fy * fy * (3.0 - 2.0 * fy)
    x, y = int(ix), int(iy)
    a, b = hash2(x, y, seed), hash2(x + 1, y, seed)
    c, d = hash2(x, y + 1, seed), hash2(x + 1, y + 1, seed)
    ab = a + (b - a) * fx
    cd = c + (d - c) * fx
    return ab + (cd - ab) * fy


def pcg(v):
    state = (v * 747796405 + 2891336453) & M32
    word = (((state >> ((state >> 28) + 4)) ^ state) * 277803737) & M32
    return ((word >> 22) ^ word) & M32


class Rng:
    def __init__(self, seed):
        self.state = ((seed & M32) * 747796405 + 2891336453) & M32

    def next(self):
        w = pcg(self.state)
        self.state = (self.state * 747796405 + 2891336453) & M32
        return (w & 0xFFFFFF) / 16777216.0

    def range(self, lo, hi):
        return lo + (hi - lo) * self.next()


# =============================================================================
# Primitives (Primitives.cpp): (positions, triangles)
# =============================================================================


class Prim:
    """positions + triangles. `uvs` (optional, per vertex): (u, v, kind) in
    metres of the unscaled primitive, kind 0 = wraps around the local Y axis
    (cylinder sides), 1 = planar on local XZ (caps). Primitives without uvs are
    box-projected per face (see Model.append)."""

    def __init__(self, positions, tris, uvs=None):
        self.positions = positions
        self.tris = tris
        self.uvs = uvs


def cube(size=(1.0, 1.0, 1.0)):
    h = v_mul(size, 0.5)
    faces = [
        ((1, 0, 0), (0, 0, -1), (0, 1, 0)), ((-1, 0, 0), (0, 0, 1), (0, 1, 0)),
        ((0, 1, 0), (1, 0, 0), (0, 0, -1)), ((0, -1, 0), (1, 0, 0), (0, 0, 1)),
        ((0, 0, 1), (1, 0, 0), (0, 1, 0)), ((0, 0, -1), (-1, 0, 0), (0, 1, 0)),
    ]
    pos, tris = [], []
    for n, u, v in faces:
        base = len(pos)
        for cx, cy in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            p = v_add(v_add(n, v_mul(u, cx)), v_mul(v, cy))
            pos.append((p[0] * h[0], p[1] * h[1], p[2] * h[2]))
        tris += [(base, base + 1, base + 2), (base, base + 2, base + 3)]
    return Prim(pos, tris)


def icosphere(radius, subdivisions=1):
    t = (1.0 + math.sqrt(5.0)) / 2.0
    pos = [(-1, t, 0), (1, t, 0), (-1, -t, 0), (1, -t, 0), (0, -1, t), (0, 1, t),
           (0, -1, -t), (0, 1, -t), (t, 0, -1), (t, 0, 1), (-t, 0, -1), (-t, 0, 1)]
    idx = [0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11, 1, 5, 9, 5, 11, 4,
           11, 10, 2, 10, 7, 6, 7, 1, 8, 3, 9, 4, 3, 4, 2, 3, 2, 6, 3, 6, 8,
           3, 8, 9, 4, 9, 5, 2, 4, 11, 6, 2, 10, 8, 6, 7, 9, 8, 1]
    pos = [v_norm(p) for p in pos]
    for _ in range(subdivisions):
        cache = {}

        def midpoint(a, b):
            key = (min(a, b), max(a, b))
            if key in cache:
                return cache[key]
            pa, pb = pos[a], pos[b]
            pos.append(v_norm(((pa[0] + pb[0]) * 0.5, (pa[1] + pb[1]) * 0.5, (pa[2] + pb[2]) * 0.5)))
            cache[key] = len(pos) - 1
            return cache[key]

        nxt = []
        for i in range(0, len(idx), 3):
            a, b, c = idx[i], idx[i + 1], idx[i + 2]
            ab, bc, ca = midpoint(a, b), midpoint(b, c), midpoint(c, a)
            nxt += [a, ab, ca, b, bc, ab, c, ca, bc, ab, bc, ca]
        idx = nxt
    return Prim([v_mul(p, radius) for p in pos], [tuple(idx[i:i + 3]) for i in range(0, len(idx), 3)])


def cylinder(r_bottom, r_top, height, segments=12, caps=True):
    pos, tris, uvs, ring = [], [], [], []
    # Sides: u = arc length around the mid-height ring, v = height (metres), so
    # the grain stays straight and the texture keeps its size. u is filled in by
    # Model.append from `ring` measured after the transform (handles squashed
    # cylinders such as the jungle tree's buttress roots).
    r_mid = (r_bottom + r_top) * 0.5
    for s in range(segments + 1):
        a = s / segments * TAU
        d = (math.cos(a), 0.0, -math.sin(a))
        pos.append(v_mul(d, r_bottom))
        pos.append(v_add(v_mul(d, r_top), (0.0, height, 0.0)))
        uvs += [(s, 0.0, 0), (s, height, 0)]
        ring.append(v_add(v_mul(d, r_mid), (0.0, height * 0.5, 0.0)))
    for s in range(segments):
        i = s * 2
        tris += [(i, i + 2, i + 1), (i + 1, i + 2, i + 3)]
    if caps:
        for y, r, up in ((0.0, r_bottom, False), (height, r_top, True)):
            if r <= 0.0:
                continue
            center = len(pos)
            pos.append((0.0, y, 0.0))
            uvs.append((0.0, 0.0, 1))
            for s in range(segments + 1):
                a = s / segments * TAU
                p = (math.cos(a) * r, y, -math.sin(a) * r)
                pos.append(p)
                uvs.append((p[0], p[2], 1))
            for s in range(segments):
                a, b = center + 1 + s, center + 2 + s
                tris.append((center, a, b) if up else (center, b, a))
    prim = Prim(pos, tris, uvs)
    prim.ring = ring
    return prim


# Organic blob: icosphere with noise displacement (Vegetation.cpp blob()).
def blob(rng, roughness, subdiv=1):
    m = icosphere(1.0, subdiv)
    seed = rng.range(0.0, 100.0)
    out = []
    for p in m.positions:
        n = value_noise(p[0] * 1.7 + seed, p[2] * 1.7 + p[1] * 1.3, 3)
        out.append(v_mul(p, 1.0 - roughness * 0.5 + roughness * n))
    m.positions = out
    return m


def vary(rng, c, amount):
    k = rng.range(1.0 - amount, 1.0 + amount)
    return (c[0] * k, c[1] * k, c[2] * k)


# =============================================================================
# Model: what MeshData accumulates, plus a material (part) per append
# =============================================================================


class Model:
    def __init__(self):
        self.verts = []
        self.tris = []
        self.colors = []      # per triangle (r, g, b), linear
        self.materials = []   # part names, in slot order
        self.tri_mat = []     # per triangle slot index
        self.uvs = []         # per triangle: three (u, v) in metres

    def slot(self, name):
        if name not in self.materials:
            self.materials.append(name)
        return self.materials.index(name)

    def append(self, prim, xf, color, material):
        """MeshData::append(prim, xf, tint). Coincident vertices inside one
        primitive (cylinder seams, cap rims, cube corners) are welded so the
        result is a clean closed mesh in Blender; triangles are unchanged."""
        slot = self.slot(material)
        weld, remap, world = {}, [], []
        for p in prim.positions:
            q = m_point(xf, p)
            world.append(q)
            key = (round(q[0], 5), round(q[1], 5), round(q[2], 5))
            if key not in weld:
                weld[key] = len(self.verts)
                self.verts.append(q)
            remap.append(weld[key])
        # How much the transform stretches the primitive's local axes (metres -> metres).
        sx, sy, sz = (math.sqrt(xf[0][k] ** 2 + xf[1][k] ** 2 + xf[2][k] ** 2) for k in range(3))
        arc = [0.0]  # cylinders: cumulative arc length of the transformed mid ring, per segment
        ring = [m_point(xf, p) for p in getattr(prim, "ring", [])]
        for i in range(1, len(ring)):
            arc.append(arc[-1] + math.dist(ring[i - 1], ring[i]))
        for a, b, c in prim.tris:
            t = (remap[a], remap[b], remap[c])
            if len(set(t)) < 3:
                continue
            self.tris.append(t)
            self.colors.append(tuple(color))
            self.tri_mat.append(slot)
            if prim.uvs is not None:
                corner = []
                for i in (a, b, c):
                    u, v, kind = prim.uvs[i]
                    corner.append((arc[int(u)], v * sy) if kind == 0 else (u * sx, v * sz))
                self.uvs.append(tuple(corner))
            else:
                self.uvs.append(_box_uvs(world[a], world[b], world[c]))


def _box_uvs(a, b, c):
    """Box (tri-planar) projection of one triangle in metres: project along the
    dominant axis of its normal, keeping 'up' as v on the side faces."""
    e1, e2 = (b[0] - a[0], b[1] - a[1], b[2] - a[2]), (c[0] - a[0], c[1] - a[1], c[2] - a[2])
    n = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
    ax = max(range(3), key=lambda k: abs(n[k]))
    sign = 1.0 if n[ax] >= 0.0 else -1.0
    if ax == 1:    # top / bottom
        return tuple((p[0], -p[2] * sign) for p in (a, b, c))
    if ax == 0:    # +-X sides
        return tuple((-p[2] * sign, p[1]) for p in (a, b, c))
    return tuple((p[0] * sign, p[1]) for p in (a, b, c))  # +-Z sides


def A(m, material, prim, xf, color):
    """m.append(prim, xf, color) with C++ argument-order semantics."""
    p, x, c = args(prim, xf, color)
    m.append(p, x, c, material)


I4 = m_identity()

# =============================================================================
# Vegetation.cpp
# =============================================================================


def acacia(seed):
    rng, m = Rng(seed), Model()
    bark, leaves = (0.30, 0.24, 0.19), (0.22, 0.30, 0.10)
    trunk_h = rng.range(2.6, 3.6)
    lean = rng.range(-0.18, 0.18)
    lean_yaw = rng.range(0.0, TAU)
    trunk_t = trs((0, 0, 0), lean_yaw, lean)
    A(m, "Bark", cylinder(0.32, 0.2, trunk_h, 7, False), trunk_t, lambda: vary(rng, bark, 0.1))
    top = m_point(trunk_t, (0, trunk_h, 0))

    branches = 3 + int(rng.next() * 2.0)
    for i in range(branches):
        yaw = (i + rng.range(-0.3, 0.3)) * TAU / branches
        tilt = rng.range(0.55, 0.95)
        length = rng.range(1.8, 2.8)
        bt = trs(top, yaw, tilt)
        A(m, "Bark", cylinder(0.14, 0.07, length, 5, False), bt, lambda: vary(rng, bark, 0.1))
        tip = m_point(bt, (0, length, 0))
        r = rng.range(1.8, 2.8)
        A(m, "Leaves", lambda: blob(rng, 0.5),
          lambda: TRS(v_add(tip, (0, 0.25, 0)), lambda: rng.range(0.0, 6.28), 0.0,
                      lambda: (r, rng.range(0.4, 0.55), r * rng.range(0.8, 1.2))),
          lambda: vary(rng, leaves, 0.15))
    crown = rng.range(3.4, 4.6)
    A(m, "Leaves", lambda: blob(rng, 0.4),
      lambda: TRS(v_add(top, (0, 1.7, 0)), lambda: rng.range(0.0, 6.28), 0.0, (crown, 0.65, crown * 0.9)),
      lambda: vary(rng, leaves, 0.1))
    return m


def baobab(seed):
    rng, m = Rng(seed), Model()
    bark, leaves = (0.56, 0.48, 0.44), (0.30, 0.38, 0.13)
    h = rng.range(6.0, 7.5)
    A(m, "Bark", cylinder(1.6, 1.15, h, 10, True), I4, lambda: vary(rng, bark, 0.06))
    branches = 6
    for i in range(branches):
        yaw = (i + rng.range(-0.2, 0.2)) * TAU / branches
        length = rng.range(1.6, 2.8)
        bt = trs(v_add((0, h - 0.3, 0), v_mul((math.cos(yaw), 0, math.sin(yaw)), 0.6)), yaw, rng.range(0.6, 1.1))
        A(m, "Bark", cylinder(0.38, 0.12, length, 6, False), bt, lambda: vary(rng, bark, 0.06))
        if rng.next() < 0.6:
            tip = m_point(bt, (0, length, 0))
            A(m, "Leaves", lambda: blob(rng, 0.5), trs(tip, 0.0, 0.0, (1.0, 0.55, 1.0)),
              lambda: vary(rng, leaves, 0.15))
    return m


def fever_tree(seed):
    rng, m = Rng(seed), Model()
    bark, leaves = (0.68, 0.68, 0.34), (0.29, 0.38, 0.14)
    h = rng.range(5.5, 8.0)
    tt = TRS((0, 0, 0), lambda: rng.range(0.0, 6.28), lambda: rng.range(-0.1, 0.1))
    A(m, "Bark", cylinder(0.22, 0.12, h, 6, False), tt, lambda: vary(rng, bark, 0.08))
    top = m_point(tt, (0, h, 0))
    for _ in range(4):
        yaw = rng.range(0.0, 6.28)
        length = rng.range(1.5, 2.6)
        base = m_point(tt, (0, h * rng.range(0.6, 0.9), 0))
        bt = trs(base, yaw, rng.range(0.4, 0.7))
        A(m, "Bark", cylinder(0.09, 0.04, length, 5, False), bt, lambda: vary(rng, bark, 0.08))
        tip = m_point(bt, (0, length, 0))
        r = rng.range(1.2, 1.9)
        A(m, "Leaves", lambda: blob(rng, 0.5),
          lambda: TRS(tip, lambda: rng.range(0.0, 6.28), 0.0, (r, 0.45, r)),
          lambda: vary(rng, leaves, 0.15))
    A(m, "Leaves", lambda: blob(rng, 0.5), trs(top, 0.0, 0.0, (2.0, 0.6, 2.0)), lambda: vary(rng, leaves, 0.1))
    return m


def bush(seed, dry, m=None):
    rng, m = Rng(seed), m or Model()
    c = (0.45, 0.40, 0.21) if dry else (0.27, 0.33, 0.13)
    for _ in range(3):
        off = vec3(lambda: rng.range(-0.7, 0.7), lambda: rng.range(0.3, 0.6), lambda: rng.range(-0.7, 0.7))
        r = rng.range(0.7, 1.2)
        A(m, "Leaves", lambda: blob(rng, 0.6),
          lambda: TRS(off, lambda: rng.range(0.0, 6.28), 0.0, lambda: (r, r * rng.range(0.6, 0.85), r)),
          lambda: vary(rng, c, 0.12))
    return m


def palm(seed):
    rng, m = Rng(seed), Model()
    bark, fronds = (0.42, 0.34, 0.25), (0.20, 0.36, 0.11)
    yaw = rng.range(0.0, TAU)
    bend = rng.range(0.05, 0.12)
    t = trs((0, 0, 0), yaw, rng.range(0.0, 0.1))
    segments = 6
    segment = rng.range(1.5, 2.0)
    for i in range(segments):
        r0 = 0.26 - 0.02 * i
        r1 = r0 - 0.02
        A(m, "Bark", cylinder(r0, r1, segment, 6, False), t, lambda: vary(rng, bark, 0.08))
        t = m_mul(t, trs((0, segment, 0), 0.0, bend))
    top = m_point(t, (0, 0, 0))
    count = 7 + int(rng.next() * 3.0)
    for i in range(count):
        a = (i + rng.range(-0.25, 0.25)) * TAU / count
        length = rng.range(2.4, 3.2)
        d = (math.cos(a), -rng.range(0.25, 0.55), math.sin(a))
        centre = v_add(top, v_mul(v_norm(d), length * 0.5))
        f = m_translate(m_identity(), centre)
        f = m_rotate(f, -a, (0, 1, 0))
        f = m_rotate(f, -math.atan(-d[1]), (0, 0, 1))
        A(m, "Fronds", lambda: blob(rng, 0.4), m_scale(f, (length * 0.5, 0.06, 0.4)),
          lambda: vary(rng, fronds, 0.15))
    for _ in range(3):  # coconuts
        A(m, "Coconut", lambda: blob(rng, 0.2),
          lambda: trs(v_add(top, vec3(lambda: rng.range(-0.25, 0.25), -0.3, lambda: rng.range(-0.25, 0.25))),
                      0.0, 0.0, (0.16, 0.16, 0.16)),
          (0.35, 0.27, 0.14))
    return m


def jungle_tree(seed):
    rng, m = Rng(seed), Model()
    bark, leaves = (0.36, 0.31, 0.24), (0.13, 0.25, 0.08)
    h = rng.range(11.0, 15.0)
    A(m, "Bark", cylinder(0.55, 0.32, h, 8, False), I4, lambda: vary(rng, bark, 0.06))
    for i in range(4):  # buttress roots
        a = (i + rng.range(-0.2, 0.2)) * HALF_PI
        r = m_translate(m_identity(), v_mul((math.cos(a), 0.0, math.sin(a)), 0.55))
        r = m_rotate(r, -a, (0, 1, 0))
        A(m, "Bark", lambda: cylinder(0.9, 0.05, rng.range(1.8, 2.6), 4, False), m_scale(r, (1.0, 1.0, 0.18)),
          lambda: vary(rng, bark, 0.06))
    top = (0.0, h, 0.0)
    for i in range(5):
        yaw = (i + rng.range(-0.3, 0.3)) * TAU / 5.0
        length = rng.range(2.5, 3.8)
        bt = TRS(lambda: (0.0, h * rng.range(0.75, 0.95), 0.0), yaw, lambda: rng.range(0.7, 1.05))
        A(m, "Bark", cylinder(0.2, 0.1, length, 5, False), bt, lambda: vary(rng, bark, 0.06))
        tip = m_point(bt, (0, length, 0))
        r = rng.range(2.4, 3.4)
        A(m, "Leaves", lambda: blob(rng, 0.5),
          lambda: TRS(v_add(tip, (0, 0.4, 0)), lambda: rng.range(0.0, 6.28), 0.0, (r, r * 0.55, r)),
          lambda: vary(rng, leaves, 0.15))
    crown = rng.range(3.6, 4.8)
    A(m, "Leaves", lambda: blob(rng, 0.45),
      lambda: TRS(v_add(top, (0, 1.2, 0)), lambda: rng.range(0.0, 6.28), 0.0, (crown, crown * 0.6, crown)),
      lambda: vary(rng, leaves, 0.1))
    return m


def undergrowth(seed):
    rng, m = Rng(seed), Model()
    c = (0.15, 0.31, 0.09)
    for _ in range(4):
        off = vec3(lambda: rng.range(-0.9, 0.9), lambda: rng.range(0.4, 0.9), lambda: rng.range(-0.9, 0.9))
        r = rng.range(0.8, 1.3)
        A(m, "Leaves", lambda: blob(rng, 0.7),
          lambda: TRS(off, lambda: rng.range(0.0, 6.28), 0.0, lambda: (r, r * rng.range(0.7, 1.0), r)),
          lambda: vary(rng, c, 0.15))
    return m


def quiver_tree(seed):
    rng, m = Rng(seed), Model()
    bark, rosette = (0.74, 0.64, 0.44), (0.36, 0.43, 0.30)
    h = rng.range(2.4, 3.4)
    A(m, "Bark", cylinder(0.42, 0.26, h, 8, False), I4, lambda: vary(rng, bark, 0.06))
    tips = [(m_translate(m_identity(), (0.0, h, 0.0)), 0.24)]
    for _ in range(2):
        nxt = []
        for tip_t, tip_r in tips:
            base = rng.range(0.0, 6.28)
            for k in range(2):
                length = rng.range(0.8, 1.3)
                bt = m_mul(tip_t, trs((0, 0, 0), base + k * math.pi, rng.range(0.45, 0.7)))
                A(m, "Bark", cylinder(tip_r, tip_r * 0.75, length, 6, False), bt, lambda: vary(rng, bark, 0.06))
                nxt.append((m_mul(bt, m_translate(m_identity(), (0.0, length, 0.0))), tip_r * 0.75))
        tips = nxt
    for tip_t, _ in tips:
        p = m_point(tip_t, (0, 0, 0))
        A(m, "Rosette", icosphere(1.0, 0),
          lambda: TRS(v_add(p, (0, 0.15, 0)), lambda: rng.range(0.0, 6.28), 0.0, (0.5, 0.4, 0.5)),
          lambda: vary(rng, rosette, 0.12))
    return m


def acacia_stump(seed):
    rng, m = Rng(seed), Model()
    bark, wood = (0.30, 0.24, 0.19), (0.72, 0.60, 0.42)
    h = rng.range(0.45, 0.65)
    A(m, "Bark", cylinder(0.36, 0.31, h, 7, False), I4, lambda: vary(rng, bark, 0.1))
    A(m, "Wood", cylinder(0.31, 0.27, 0.04, 7, True), trs((0.0, h, 0.0), 0.0, 0.0), lambda: vary(rng, wood, 0.05))
    for i in range(3):
        a = (i + rng.range(-0.2, 0.2)) * TAU / 3.0
        A(m, "Bark", lambda: cylinder(0.12, 0.04, rng.range(0.5, 0.8), 5, False),
          trs(v_mul((math.cos(a), 0.05, math.sin(a)), 0.25), -a + HALF_PI, 1.35),
          lambda: vary(rng, bark, 0.1))
    return m


def berry_bush(seed, with_berries):
    m = bush(seed, False)
    if not with_berries:
        return m
    rng = Rng((seed * 7 + 3) & M32)
    for _ in range(14):
        d = v_norm(vec3(lambda: rng.range(-1.0, 1.0), lambda: rng.range(0.1, 1.0), lambda: rng.range(-1.0, 1.0)))
        A(m, "Berries", icosphere(0.09, 0),
          lambda: trs(v_add(v_mul(d, rng.range(0.9, 1.25)), (0.0, 0.45, 0.0)), 0.0, 0.0),
          lambda: vary(rng, (0.62, 0.08, 0.10), 0.12))
    return m


def rock(seed, flatness):
    rng, m = Rng(seed), Model()
    A(m, "Rock", lambda: blob(rng, 0.7, 2),
      lambda: trs((0, 0.25, 0), 0.0, 0.0, (rng.range(1.0, 1.5), flatness, rng.range(0.8, 1.2))),
      lambda: vary(rng, (0.49, 0.45, 0.40), 0.1))
    return m


# =============================================================================
# Inventory.cpp itemMesh() and Props.cpp (no randomness)
# =============================================================================


def _at(p, yaw=0.0, roll=0.0):
    t = m_translate(m_identity(), p)
    t = m_rotate(t, yaw, (0, 1, 0))
    return m_rotate(t, roll, (0, 0, 1))


def item_wood(_seed=0):
    m = Model()
    bark, cut = (0.36, 0.24, 0.15), (0.80, 0.64, 0.42)
    for p in ((-0.23, 0.2, 0.0), (0.23, 0.2, 0.0), (0.0, 0.58, 0.0)):
        t = _at(p, math.radians(-25.0), math.radians(90.0))
        m.append(cylinder(0.22, 0.22, 1.6, 9, False), t, bark, "Bark")
        m.append(cylinder(0.2, 0.2, 0.02, 9, True), m_translate(t, (0.0, 0.8, 0.0)), cut, "Wood")
        m.append(cylinder(0.2, 0.2, 0.02, 9, True), m_translate(t, (0.0, -0.81, 0.0)), cut, "Wood")
    return m


def item_berries(_seed=0):
    m = Model()
    pos = [(0, 0, 0), (0.3, 0.05, 0.1), (-0.28, 0.03, 0.12), (0.08, 0.04, -0.3),
           (0.15, 0.3, -0.05), (-0.15, 0.28, 0.05), (0.0, 0.55, 0.0)]
    for i, p in enumerate(pos):
        m.append(icosphere(0.2, 2), _at(p), (0.72 + 0.04 * (i % 3), 0.08, 0.12), "Berries")
    m.append(cylinder(0.025, 0.02, 0.4, 6), _at((0.0, 0.75, 0.0)), (0.3, 0.35, 0.15), "Stem")
    m.append(cube((0.45, 0.02, 0.2)), _at((0.2, 0.85, 0.0), 0.0, math.radians(25.0)), (0.3, 0.5, 0.2), "Leaves")
    m.append(cube((0.45, 0.02, 0.2)), _at((-0.2, 0.85, 0.0), 0.0, math.radians(-25.0)), (0.28, 0.46, 0.18), "Leaves")
    return m


def item_stone(_seed=0):
    m = Model()
    m.append(icosphere(0.5, 1), m_scale(_at((0.0, 0.28, 0.0)), (1.15, 0.6, 0.9)), (0.47, 0.44, 0.40), "Rock")
    return m


def item_stick(_seed=0):
    m = Model()
    bark = (0.42, 0.30, 0.19)
    m.append(cylinder(0.09, 0.06, 1.6, 6), _at((0.0, 0.09, 0.0), 0.0, math.radians(90.0)), bark, "Bark")
    m.append(cylinder(0.05, 0.025, 0.5, 5),
             m_translate(_at((0.25, 0.09, 0.12), math.radians(-40.0), math.radians(90.0)), (0.0, 0.25, 0.0)),
             bark, "Bark")
    return m


def item_sunstone(_seed=0):
    m = Model()
    m.append(icosphere(0.55, 0), m_scale(m_identity(), (1.0, 1.25, 1.0)), (1.0, 0.72, 0.25), "Gem")
    return m


def campfire(_seed=0):
    m = Model()
    for i in range(10):
        a = i / 10.0 * TAU
        t = m_translate(m_identity(), (math.cos(a) * 0.9, 0.1, math.sin(a) * 0.9))
        m.append(icosphere(1.0, 1), m_scale(t, (0.28, 0.2, 0.24)), (0.38, 0.36, 0.33), "Rock")
    for i in range(4):
        a = i / 4.0 * math.pi
        t = m_translate(m_identity(), (math.cos(a) * 0.55, 0.1, math.sin(a) * 0.55))
        t = m_rotate(t, -a, (0, 1, 0))
        t = m_rotate(t, math.radians(70.0), (0, 0, 1))
        m.append(cylinder(0.08, 0.07, 1.0, 6), t, (0.22, 0.14, 0.08), "Bark")
    return m


def torch(_seed=0):
    m = Model()
    m.append(cylinder(0.05, 0.04, 1.9, 6), m_identity(), (0.25, 0.16, 0.09), "Bark")
    m.append(cylinder(0.09, 0.11, 0.25, 6), m_translate(m_identity(), (0, 1.85, 0)), (0.12, 0.09, 0.07), "TorchHead")
    return m


def sunstone_pickup(_seed=0):
    m = Model()
    m.append(icosphere(0.22, 0), m_identity(), (1.0, 0.6, 0.25), "Gem")
    return m


# =============================================================================
# Registry: the seeds the game registers (Vegetation.cpp registerPropMeshes())
# =============================================================================

# key: (label, category, builder(seed), registered seeds, description)
KINDS = {
    "acacia": ("Acacia", "Savanna", acacia, [100 + i for i in range(5)], "Flat-crowned acacia tree"),
    "baobab": ("Baobab", "Savanna", baobab, [200 + i for i in range(2)], "Fat-trunked baobab"),
    "fever_tree": ("Fever Tree", "Savanna", fever_tree, [300 + i for i in range(3)], "Tall slim fever tree"),
    "bush": ("Bush", "Savanna", lambda s: bush(s, (s - 400) % 2 == 0), [400 + i for i in range(4)],
             "Bush (even variants are dry, as in the game)"),
    "rock": ("Rock", "Savanna", lambda s: rock(s, 0.75), [500 + i for i in range(4)], "Small rock"),
    "boulder": ("Boulder", "Savanna", lambda s: rock(s, 1.1), [600 + i for i in range(3)], "Boulder"),
    "acacia_stump": ("Acacia Stump", "Interaction", acacia_stump, [1100], "Felled acacia stump"),
    "berry_bush": ("Berry Bush", "Interaction", lambda s: berry_bush(s, True), [1200, 1201], "Bush with berries"),
    "berry_bush_picked": ("Berry Bush (picked)", "Interaction", lambda s: berry_bush(s, False), [1200, 1201],
                          "Berry bush after harvesting"),
    "dry_bush": ("Dry Bush", "Desert", lambda s: bush(s, True), [450 + i for i in range(3)], "Dry desert bush"),
    "quiver_tree": ("Quiver Tree", "Desert", quiver_tree, [1000 + i for i in range(3)], "Forking quiver tree"),
    "palm": ("Palm", "Jungle", palm, [700 + i for i in range(3)], "Curved palm with fronds"),
    "jungle_tree": ("Jungle Tree", "Jungle", jungle_tree, [800 + i for i in range(3)], "Rainforest giant"),
    "undergrowth": ("Undergrowth", "Jungle", undergrowth, [900 + i for i in range(3)], "Leafy undergrowth clump"),
    "item_wood": ("Item: Wood", "Items", item_wood, [0], "Inventory wood (three logs)"),
    "item_berries": ("Item: Berries", "Items", item_berries, [0], "Inventory berries"),
    "item_stone": ("Item: Stone", "Items", item_stone, [0], "Inventory stone"),
    "item_stick": ("Item: Stick", "Items", item_stick, [0], "Inventory stick"),
    "item_sunstone": ("Item: Sunstone", "Items", item_sunstone, [0], "Inventory sunstone gem"),
    "campfire": ("Campfire", "Props", campfire, [0], "Stone ring with crossed logs"),
    "torch": ("Torch", "Props", torch, [0], "Standing torch"),
    "sunstone_pickup": ("Sunstone Pickup", "Props", sunstone_pickup, [0], "World sunstone pickup"),
}

CATEGORY_ORDER = ["Savanna", "Jungle", "Desert", "Interaction", "Items", "Props"]


def build(kind, seed, right_to_left=True):
    global _RIGHT_TO_LEFT
    _RIGHT_TO_LEFT = right_to_left
    return KINDS[kind][2](seed)


# =============================================================================
# Blender side
# =============================================================================

# Defaults from propMaterial() (Props.cpp): white base, roughness 0.85, vertex colour weight 1.
MATERIAL_ROUGHNESS = {"Gem": 0.3}
MATERIAL_EMISSION = {"Gem": ((1.0, 0.45, 0.12), 6.0)}


def _engine_to_blender(p):
    # Y-up -> Z-up, as Blender's glTF importer does.
    return (p[0], -p[2], p[1])


def _material(name, mean_color, color_mode):
    mat_name = "VE_" + name
    mat = bpy.data.materials.get(mat_name)
    if mat is not None:
        return mat
    mat = bpy.data.materials.new(mat_name)
    mat.use_nodes = True
    nt = mat.node_tree
    bsdf = next((n for n in nt.nodes if n.type == "BSDF_PRINCIPLED"), None)
    if bsdf is None:
        return mat
    bsdf.inputs["Roughness"].default_value = MATERIAL_ROUGHNESS.get(name, 0.85)
    if color_mode == "VERTEX":
        attr = nt.nodes.new("ShaderNodeVertexColor")
        attr.layer_name = "Col"
        attr.location = (bsdf.location.x - 300, bsdf.location.y)
        nt.links.new(attr.outputs["Color"], bsdf.inputs["Base Color"])
    else:
        bsdf.inputs["Base Color"].default_value = (*mean_color, 1.0)
    mat.diffuse_color = (*mean_color, 1.0)  # solid viewport colour
    if name in MATERIAL_EMISSION:
        col, strength = MATERIAL_EMISSION[name]
        sock = bsdf.inputs.get("Emission Color") or bsdf.inputs.get("Emission")
        if sock is not None:
            sock.default_value = (*col, 1.0)
        if "Emission Strength" in bsdf.inputs:
            bsdf.inputs["Emission Strength"].default_value = strength
    return mat


def model_to_object(context, model, name, collection, location, color_mode, uv_mode="TILING", uv_tile=1.0):
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata([_engine_to_blender(v) for v in model.verts], [], model.tris)

    # UVs, per corner (from_pydata keeps each triangle's corner order).
    if uv_mode != "NONE":
        layer = mesh.uv_layers.new(name="UVMap")
        inv = 1.0 / max(uv_tile, 1e-6)
        flat = [0.0] * (len(mesh.loops) * 2)
        for poly in mesh.polygons:
            for k, (u, v) in enumerate(model.uvs[poly.index]):
                li = poly.loop_start + k
                flat[li * 2] = u * inv
                flat[li * 2 + 1] = v * inv
        layer.data.foreach_set("uv", flat)

    # Per-face tint as a corner colour attribute ("Col" -> glTF COLOR_0).
    attr = mesh.color_attributes.new("Col", "FLOAT_COLOR", "CORNER")
    flat = [0.0] * (len(mesh.loops) * 4)
    for poly in mesh.polygons:
        r, g, b = model.colors[poly.index]
        for li in range(poly.loop_start, poly.loop_start + poly.loop_total):
            flat[li * 4:li * 4 + 4] = (r, g, b, 1.0)
    attr.data.foreach_set("color", flat)

    # One material slot per part.
    for slot, part in enumerate(model.materials):
        cols = [c for c, s in zip(model.colors, model.tri_mat) if s == slot]
        mean = tuple(sum(c[i] for c in cols) / len(cols) for i in range(3)) if cols else (0.8, 0.8, 0.8)
        mesh.materials.append(_material(part, mean, color_mode))
    mesh.polygons.foreach_set("material_index", model.tri_mat)
    mesh.polygons.foreach_set("use_smooth", [False] * len(mesh.polygons))
    mesh.validate(clean_customdata=False)  # after the attributes, so any removed face takes its data along
    mesh.update()

    obj = bpy.data.objects.new(name, mesh)
    obj.location = location
    collection.objects.link(obj)
    if uv_mode == "UNWRAP":
        _smart_unwrap(context, obj)
    return obj


def _smart_unwrap(context, obj):
    """Non-overlapping 0-1 layout (for hand painting / baking)."""
    view_layer = context.view_layer
    prev_active = view_layer.objects.active
    prev_selected = [o for o in context.selected_objects]
    for o in prev_selected:
        o.select_set(False)
    obj.select_set(True)
    view_layer.objects.active = obj
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.uv.smart_project(angle_limit=math.radians(66.0), island_margin=0.01)
    bpy.ops.object.mode_set(mode="OBJECT")
    obj.select_set(False)
    for o in prev_selected:
        o.select_set(True)
    view_layer.objects.active = prev_active


def _collection(context, name):
    coll = bpy.data.collections.get(name)
    if coll is None:
        coll = bpy.data.collections.new(name)
        context.scene.collection.children.link(coll)
    return coll


if bpy is not None:

    ORDER_ITEMS = [
        ("RTL", "Right-to-left (GCC / MSVC)", "Matches the game built with GCC (MinGW) or MSVC"),
        ("LTR", "Left-to-right (Clang)", "Matches the game built with Clang"),
    ]
    COLOR_ITEMS = [
        ("VERTEX", "Vertex colours", "Materials read the per-face tint from the 'Col' attribute (like the engine)"),
        ("MATERIAL", "Material colours", "Each part material gets its average tint as a flat base colour"),
    ]

    UV_ITEMS = [
        ("TILING", "Tiling (real-world)", "Cylinders wrap around, everything else is box-projected; "
                                          "UV size is in metres, so tileable textures match across props"),
        ("UNWRAP", "Unwrapped (0-1)", "Smart UV Project: a non-overlapping 0-1 layout for painting / baking"),
        ("NONE", "None", "No UV map"),
    ]

    def _common_props(cls):
        cls.__annotations__["arg_order"] = EnumProperty(name="Argument order", items=ORDER_ITEMS, default="RTL")
        cls.__annotations__["color_mode"] = EnumProperty(name="Colour", items=COLOR_ITEMS, default="VERTEX")
        cls.__annotations__["uv_mode"] = EnumProperty(name="UVs", items=UV_ITEMS, default="TILING")
        cls.__annotations__["uv_tile"] = FloatProperty(
            name="Texture size", default=1.0, min=0.01, unit="LENGTH",
            description="Tiling UVs: metres covered by one repeat of the texture")
        return cls

    def _draw_common(op, col):
        col.prop(op, "arg_order")
        col.prop(op, "color_mode")
        col.prop(op, "uv_mode")
        if op.uv_mode == "TILING":
            col.prop(op, "uv_tile")

    @_common_props
    class VE_OT_add_prop(bpy.types.Operator):
        """Add one of the sandbox's procedural props at the 3D cursor"""
        bl_idname = "mesh.ve_add_prop"
        bl_label = "VE Prop"
        bl_options = {"REGISTER", "UNDO"}

        kind: EnumProperty(name="Prop", items=[(k, v[0], v[4]) for k, v in KINDS.items()])
        variant: IntProperty(name="Variant", default=0, min=0,
                             description="Index into the seeds the game registers for this prop")
        use_custom_seed: BoolProperty(name="Custom seed", default=False,
                                      description="Use any seed instead of one the game registers")
        seed: IntProperty(name="Seed", default=100, min=0)

        def draw(self, context):
            col = self.layout.column()
            col.prop(self, "kind")
            seeds = KINDS[self.kind][3]
            row = col.row()
            row.enabled = not self.use_custom_seed and len(seeds) > 1
            row.prop(self, "variant", text=f"Variant (of {len(seeds)})")
            col.prop(self, "use_custom_seed")
            if self.use_custom_seed:
                col.prop(self, "seed")
            _draw_common(self, col)

        def execute(self, context):
            label, _, _, seeds, _ = KINDS[self.kind]
            seed = self.seed if self.use_custom_seed else seeds[min(self.variant, len(seeds) - 1)]
            model = build(self.kind, seed, self.arg_order == "RTL")
            name = f"{self.kind}_{seed}" if len(seeds) > 1 or self.use_custom_seed else self.kind
            obj = model_to_object(context, model, name, context.collection, context.scene.cursor.location.copy(),
                                  self.color_mode, self.uv_mode, self.uv_tile)
            for o in context.selected_objects:
                o.select_set(False)
            obj.select_set(True)
            context.view_layer.objects.active = obj
            obj["ve_kind"] = self.kind
            obj["ve_seed"] = seed
            self.report({"INFO"}, f"{label} seed {seed}: {len(model.tris)} triangles")
            return {"FINISHED"}

    @_common_props
    class VE_OT_add_all_props(bpy.types.Operator):
        """Add every variant of every procedural prop, laid out in rows by biome"""
        bl_idname = "mesh.ve_add_all_props"
        bl_label = "Generate All VE Props"
        bl_options = {"REGISTER", "UNDO"}

        spacing: FloatProperty(name="Gap", default=2.0, min=0.0, unit="LENGTH")

        def execute(self, context):
            root = _collection(context, "VE Props")
            rtl = self.arg_order == "RTL"
            y = 0.0
            total = 0
            for category in CATEGORY_ORDER:
                coll = bpy.data.collections.get("VE " + category)
                if coll is None:
                    coll = bpy.data.collections.new("VE " + category)
                    root.children.link(coll)
                x = 0.0
                row_depth = 0.0
                for kind, (label, cat, _, seeds, _) in KINDS.items():
                    if cat != category:
                        continue
                    for seed in seeds:
                        model = build(kind, seed, rtl)
                        bv = [_engine_to_blender(v) for v in model.verts]
                        min_x, max_x = min(v[0] for v in bv), max(v[0] for v in bv)
                        min_y, max_y = min(v[1] for v in bv), max(v[1] for v in bv)
                        name = f"{kind}_{seed}" if len(seeds) > 1 else kind
                        obj = model_to_object(context, model, name, coll, (x - min_x, y - min_y, 0.0),
                                              self.color_mode, self.uv_mode, self.uv_tile)
                        obj["ve_kind"] = kind
                        obj["ve_seed"] = seed
                        x += (max_x - min_x) + self.spacing
                        row_depth = max(row_depth, max_y - min_y)
                        total += 1
                y += row_depth + self.spacing * 2.0
            self.report({"INFO"}, f"Generated {total} props")
            return {"FINISHED"}

    class VE_PT_props(bpy.types.Panel):
        bl_label = "VE Procedural Props"
        bl_space_type = "VIEW_3D"
        bl_region_type = "UI"
        bl_category = "VE Props"

        def draw(self, context):
            layout = self.layout
            layout.label(text="Single prop (at 3D cursor):")
            col = layout.column(align=True)
            for category in CATEGORY_ORDER:
                col.label(text=category)
                grid = col.grid_flow(columns=2, align=True)
                for kind, (label, cat, _, seeds, _) in KINDS.items():
                    if cat != category:
                        continue
                    op = grid.operator(VE_OT_add_prop.bl_idname, text=label.replace("Item: ", ""))
                    op.kind = kind
            layout.separator()
            layout.operator(VE_OT_add_all_props.bl_idname, icon="OUTLINER_COLLECTION")
            layout.label(text="Tweak variant/seed in the Adjust Last", icon="INFO")
            layout.label(text="Operation panel after adding.")

    def _menu(self, context):
        self.layout.operator(VE_OT_add_prop.bl_idname, icon="OUTLINER_OB_MESH")

    _CLASSES = (VE_OT_add_prop, VE_OT_add_all_props, VE_PT_props)

    def register():
        for c in _CLASSES:
            bpy.utils.register_class(c)
        bpy.types.VIEW3D_MT_mesh_add.append(_menu)

    def unregister():
        bpy.types.VIEW3D_MT_mesh_add.remove(_menu)
        for c in reversed(_CLASSES):
            bpy.utils.unregister_class(c)


# =============================================================================
# Self-test (plain Python): python ve_procedural_props.py
# =============================================================================

if __name__ == "__main__" and bpy is None:
    for kind, (label, _, _, seeds, _) in KINDS.items():
        counts = []
        for seed in seeds:
            m = build(kind, seed, True)
            ys = [v[1] for v in m.verts]
            counts.append(f"{len(m.tris)} tris, h {max(ys) - min(ys):.2f}m")
        print(f"{label:22s} " + " | ".join(counts))
elif __name__ == "__main__":
    register()
