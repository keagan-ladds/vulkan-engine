#!/usr/bin/env python3
"""
Generates the sandbox's test assets so the repository needs no binary
downloads (and so the engine's importers get exercised on real files):

  sandbox/assets/textures/ground_detail.png   tileable grey noise (terrain detail)
  sandbox/assets/textures/mud_wall.png        red-clay wall texture
  sandbox/assets/textures/thatch.png          straw roof texture
  sandbox/assets/textures/reed_screen.png     woven reed screen with holes (alpha cutout test)
  sandbox/assets/textures/ground/*.png        terrain splat materials (soil, sand, leaf litter,
                                              rock): albedo (alpha = height), normal, roughness, AO
  sandbox/assets/models/rondavel.gltf (+.bin) round African hut: walls + thatched
                                              roof (one mesh, two materials) + door node
  sandbox/assets/audio/*.wav                  wind / fire / rain loops, pickup chime

Requires Pillow (python3-pil) and numpy (terrain materials). Run from anywhere:
    python3 tools/generate_assets.py
"""
import base64
import json
import math
import os
import random
import struct
import wave

from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "sandbox", "assets")
random.seed(1234)


def out(*parts):
    path = os.path.join(ROOT, *parts)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    return path


# --------------------------------------------------------------------------- noise
def value_noise_grid(size, cells, seed):
    rnd = random.Random(seed)
    grid = [[rnd.random() for _ in range(cells)] for _ in range(cells)]

    def sample(x, y):  # x, y in [0, size) ; tileable
        fx, fy = x / size * cells, y / size * cells
        x0, y0 = int(fx) % cells, int(fy) % cells
        x1, y1 = (x0 + 1) % cells, (y0 + 1) % cells
        tx, ty = fx - int(fx), fy - int(fy)
        tx, ty = tx * tx * (3 - 2 * tx), ty * ty * (3 - 2 * ty)
        a = grid[y0][x0] * (1 - tx) + grid[y0][x1] * tx
        b = grid[y1][x0] * (1 - tx) + grid[y1][x1] * tx
        return a * (1 - ty) + b * ty

    return sample


def fbm_image(size, octaves, base_cells, seed):
    layers = [value_noise_grid(size, base_cells << o, seed + o) for o in range(octaves)]
    data = []
    for y in range(size):
        row = []
        for x in range(size):
            v, amp, norm = 0.0, 1.0, 0.0
            for layer in layers:
                v += layer(x, y) * amp
                norm += amp
                amp *= 0.5
            row.append(v / norm)
        data.append(row)
    return data


def textures():
    size = 256
    n = fbm_image(size, 5, 4, 10)
    img = Image.new("RGB", (size, size))
    for y in range(size):
        for x in range(size):
            g = int(max(0, min(255, (n[y][x] - 0.5) * 1.6 * 255 + 128)))
            img.putpixel((x, y), (g, g, g))
    img.save(out("textures", "ground_detail.png"))

    size = 128
    n = fbm_image(size, 4, 4, 20)
    img = Image.new("RGB", (size, size))
    for y in range(size):
        for x in range(size):
            v = n[y][x]
            band = 0.06 * math.sin(y * 0.35 + v * 4.0)  # horizontal plaster strokes
            r = 0.62 + (v - 0.5) * 0.35 + band
            img.putpixel((x, y), tuple(int(max(0, min(1, c)) * 255) for c in (r, r * 0.58, r * 0.40)))
    img.save(out("textures", "mud_wall.png"))

    img = Image.new("RGB", (size, size))
    rnd = random.Random(5)
    straws = [(rnd.random() * size, rnd.random() * 0.3 + 0.7, rnd.random() * 6.28) for _ in range(140)]
    for y in range(size):
        for x in range(size):
            v = n[(y * 3) % size][x] * 0.5 + 0.5
            for sx, bright, ph in straws[:40]:
                d = abs(((x - sx + size / 2) % size) - size / 2)
                if d < 1.2:
                    v = max(v, bright)
            wave_ = 0.08 * math.sin(x * 0.8 + math.sin(y * 0.2) * 2.0)
            r = 0.55 * v + 0.12 + wave_
            img.putpixel((x, y), tuple(int(max(0, min(1, c)) * 255) for c in (r, r * 0.82, r * 0.48)))
    img.save(out("textures", "thatch.png"))


def reed_screen():
    """Woven reeds with gaps: RGBA, alpha 0 in the holes (alpha-tested material + shadows)."""
    size = 128
    img = Image.new("RGBA", (size, size))
    rnd = random.Random(9)
    for y in range(size):
        for x in range(size):
            vertical = (x % 16) < 9            # vertical reeds
            horizontal = (y % 32) < 6          # binding strips
            wobble = math.sin(y * 0.3 + (x // 16) * 1.7) * 0.06
            if vertical or horizontal:
                v = 0.62 + wobble + rnd.uniform(-0.04, 0.04) - (0.12 if horizontal else 0.0)
                img.putpixel((x, y), (int(v * 230), int(v * 190), int(v * 110), 255))
            else:
                img.putpixel((x, y), (60, 45, 25, 0))
    img.save(out("textures", "reed_screen.png"))


# --------------------------------------------------------------------------- glTF
class GltfBuilder:
    def __init__(self):
        self.bin = bytearray()
        self.buffer_views, self.accessors, self.meshes = [], [], []

    def _view(self, data, target):
        while len(self.bin) % 4:
            self.bin.append(0)
        self.buffer_views.append({"buffer": 0, "byteOffset": len(self.bin), "byteLength": len(data), "target": target})
        self.bin.extend(data)
        return len(self.buffer_views) - 1

    def accessor(self, values, comps, is_index=False):
        if is_index:
            data = struct.pack("<%dI" % len(values), *values)
            view = self._view(data, 34963)
            acc = {"bufferView": view, "componentType": 5125, "count": len(values), "type": "SCALAR"}
        else:
            flat = [c for v in values for c in v]
            data = struct.pack("<%df" % len(flat), *flat)
            view = self._view(data, 34962)
            acc = {"bufferView": view, "componentType": 5126, "count": len(values),
                   "type": {2: "VEC2", 3: "VEC3", 4: "VEC4"}[comps]}
            if comps == 3:
                acc["min"] = [min(v[i] for v in values) for i in range(3)]
                acc["max"] = [max(v[i] for v in values) for i in range(3)]
        self.accessors.append(acc)
        return len(self.accessors) - 1

    def primitive(self, pos, nrm, uv, idx, material):
        return {"attributes": {"POSITION": self.accessor(pos, 3), "NORMAL": self.accessor(nrm, 3),
                               "TEXCOORD_0": self.accessor(uv, 2)},
                "indices": self.accessor(idx, 1, True), "material": material}


def cylinder(r0, r1, y0, y1, seg, uv_repeat, gap=None):
    """Open cylinder/cone wall. `gap` = (start, end) angle in radians left open (doorway)."""
    pos, nrm, uv, idx = [], [], [], []
    slope = (r0 - r1) / (y1 - y0)
    for i in range(seg + 1):
        a = i / seg * 2 * math.pi
        c, s = math.cos(a), -math.sin(a)
        n = (c, slope, s)
        l = math.sqrt(sum(k * k for k in n))
        n = tuple(k / l for k in n)
        pos += [(c * r0, y0, s * r0), (c * r1, y1, s * r1)]
        nrm += [n, n]
        uv += [(i / seg * uv_repeat, 1.0), (i / seg * uv_repeat, 0.0)]
    for i in range(seg):
        mid = (i + 0.5) / seg * 2 * math.pi
        if gap and gap[0] < mid < gap[1]:
            continue
        a = i * 2
        idx += [a, a + 2, a + 1, a + 1, a + 2, a + 3]
    return pos, nrm, uv, idx


def box(sx, sy, sz):
    pos, nrm, uv, idx = [], [], [], []
    faces = [((1, 0, 0), (0, 0, -1), (0, 1, 0)), ((-1, 0, 0), (0, 0, 1), (0, 1, 0)),
             ((0, 1, 0), (1, 0, 0), (0, 0, -1)), ((0, -1, 0), (1, 0, 0), (0, 0, 1)),
             ((0, 0, 1), (1, 0, 0), (0, 1, 0)), ((0, 0, -1), (-1, 0, 0), (0, 1, 0))]
    h = (sx / 2, sy / 2, sz / 2)
    for n, u, v in faces:
        base = len(pos)
        for cu, cv, tu, tv in ((-1, -1, 0, 1), (1, -1, 1, 1), (1, 1, 1, 0), (-1, 1, 0, 0)):
            pos.append(tuple((n[k] + u[k] * cu + v[k] * cv) * h[k] for k in range(3)))
            nrm.append(n)
            uv.append((tu, tv))
        idx += [base, base + 1, base + 2, base, base + 2, base + 3]
    return pos, nrm, uv, idx


def rondavel():
    g = GltfBuilder()
    door_gap = (math.radians(255), math.radians(285))  # opening facing -Z... (angle measured from +X towards -Z)
    walls = cylinder(2.6, 2.6, 0.0, 2.3, 24, 6.0, gap=door_gap)
    # Inside of the wall (so the doorway doesn't reveal an invisible back face).
    wp, wn, wu, wi = walls
    inner_base = len(wp)
    wp2 = wp + [(x * 0.92, y, z * 0.92) for x, y, z in wp]
    wn2 = wn + [(-x, -y, -z) for x, y, z in wn]
    wu2 = wu + wu
    wi2 = wi + [inner_base + i for tri in zip(wi[0::3], wi[2::3], wi[1::3]) for i in tri]
    roof = cylinder(3.3, 0.0, 2.1, 4.6, 24, 4.0)
    door = box(0.9, 1.9, 0.12)

    prim_walls = g.primitive(wp2, wn2, wu2, wi2, 0)
    prim_roof = g.primitive(*roof, 1)
    g.meshes.append({"name": "Rondavel", "primitives": [prim_walls, prim_roof]})
    g.meshes.append({"name": "Door", "primitives": [g.primitive(*door, 2)]})

    gltf = {
        "asset": {"version": "2.0", "generator": "vulkan-engine tools/generate_assets.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [
            {"name": "Rondavel", "children": [1, 2]},
            {"name": "Body", "mesh": 0},
            {"name": "Door", "mesh": 1, "translation": [0.0, 0.95, 2.62]},
        ],
        "meshes": g.meshes,
        "materials": [
            {"name": "MudWall", "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "metallicFactor": 0.0,
                                                          "roughnessFactor": 0.95}},
            {"name": "Thatch", "pbrMetallicRoughness": {"baseColorTexture": {"index": 1}, "metallicFactor": 0.0,
                                                         "roughnessFactor": 1.0}, "doubleSided": True},
            {"name": "Wood", "pbrMetallicRoughness": {"baseColorFactor": [0.25, 0.15, 0.08, 1.0],
                                                       "metallicFactor": 0.0, "roughnessFactor": 0.8}},
        ],
        "textures": [{"source": 0, "sampler": 0}, {"source": 1, "sampler": 0}],
        "images": [{"uri": "../textures/mud_wall.png"}, {"uri": "../textures/thatch.png"}],
        "samplers": [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}],
        "buffers": [{"uri": "rondavel.bin", "byteLength": len(g.bin)}],
        "bufferViews": g.buffer_views,
        "accessors": g.accessors,
    }
    with open(out("models", "rondavel.bin"), "wb") as f:
        f.write(g.bin)
    with open(out("models", "rondavel.gltf"), "w") as f:
        json.dump(gltf, f, indent=1)


# --------------------------------------------------------------------------- audio
RATE = 22050


def write_wav(name, samples):
    with wave.open(out("audio", name), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(max(-1.0, min(1.0, s)) * 32000)) for s in samples))


def loop_fade(samples, fade=2000):
    """Crossfade the end into the start so the loop is seamless."""
    n = len(samples)
    res = samples[: n - fade]
    for i in range(fade):
        t = i / fade
        res[i] = res[i] * t + samples[n - fade + i] * (1 - t)
    return res


def audio():
    rnd = random.Random(7)
    n = RATE * 6
    # Wind: low-passed noise with slow gusts.
    lp, lp2, s = 0.0, 0.0, []
    for i in range(n):
        lp += (rnd.uniform(-1, 1) - lp) * 0.02
        lp2 += (lp - lp2) * 0.05
        gust = 0.55 + 0.45 * math.sin(i / RATE * 0.9) * math.sin(i / RATE * 0.37 + 1)
        s.append(lp2 * 6.0 * gust)
    write_wav("wind_loop.wav", loop_fade(s))

    # Fire: brown-noise roar + random crackles.
    lp, s = 0.0, []
    crack = 0.0
    for i in range(n):
        lp += (rnd.uniform(-1, 1) - lp) * 0.03
        if rnd.random() < 0.0009:
            crack = rnd.uniform(0.4, 0.9)
        crack *= 0.993
        s.append(lp * 1.6 + crack * rnd.uniform(-1, 1))
    write_wav("fire_loop.wav", loop_fade(s))

    # Rain: bright noise with droplet ticks.
    hp_prev, s = 0.0, []
    for i in range(n):
        w = rnd.uniform(-1, 1)
        hp = w - hp_prev * 0.6
        hp_prev = w
        tick = rnd.uniform(-1, 1) * 0.6 if rnd.random() < 0.002 else 0.0
        s.append(hp * 0.18 + tick)
    write_wav("rain_loop.wav", loop_fade(s))

    # Pickup chime: two quick sine notes.
    s = []
    for i in range(int(RATE * 0.6)):
        t = i / RATE
        note = 880.0 if t < 0.12 else 1318.5
        env = math.exp(-((t if t < 0.12 else t - 0.12)) * 9.0)
        s.append(0.5 * env * math.sin(2 * math.pi * note * t) + 0.15 * env * math.sin(4 * math.pi * note * t))
    write_wav("pickup.wav", s)


def lut_neutral(size=32):
    """Identity colour-grading LUT strip (size slices of size x size) for
    vfx::ColorGradingPass. Grade a screenshot in a photo editor, apply the
    same adjustments to this image and load it as an UNORM (non-sRGB) texture."""
    img = Image.new("RGB", (size * size, size))
    px = img.load()
    for b in range(size):
        for g in range(size):
            for r in range(size):
                px[b * size + r, g] = (round(r * 255 / (size - 1)), round(g * 255 / (size - 1)), round(b * 255 / (size - 1)))
    img.save(out("textures", f"lut_neutral_{size}.png"))


# --------------------------------------------------------------------------- terrain materials
def terrain_textures(size=512):
    """Tileable splat materials. Everything starts from a height field; the
    normal and AO maps are derived from it, so the four maps agree."""
    import numpy as np

    def spectral(seed, beta, lo=1.0, hi=None):
        # Periodic noise with a 1/f^beta spectrum between frequencies lo..hi (cycles per tile).
        rng = np.random.default_rng(seed)
        f = np.fft.fftfreq(size) * size
        fx, fy = np.meshgrid(f, f)
        r = np.hypot(fx, fy)
        amp = np.where((r >= lo) & ((r <= hi) if hi else True), 1.0 / np.maximum(r, 1.0) ** beta, 0.0)
        n = np.real(np.fft.ifft2(np.fft.fft2(rng.standard_normal((size, size))) * amp))
        return (n - n.min()) / (n.max() - n.min())

    def blur(a, radius):
        f = np.fft.fftfreq(size)
        fx, fy = np.meshgrid(f, f)
        return np.real(np.fft.ifft2(np.fft.fft2(a) * np.exp(-(fx ** 2 + fy ** 2) * (2 * np.pi * radius) ** 2 / 2)))

    yy, xx = np.mgrid[0:size, 0:size].astype(np.float64)

    def cells(seed, count):
        # Worley noise (wrapped): distance to the nearest and second nearest point, and the nearest's id.
        rng = np.random.default_rng(seed)
        pts = rng.random((count, 2)) * size
        d1 = np.full((size, size), 1e9)
        d2 = np.full((size, size), 1e9)
        ids = np.zeros((size, size), dtype=np.int32)
        for k, (px, py) in enumerate(pts):
            dx = np.abs(xx - px)
            dy = np.abs(yy - py)
            d = np.hypot(np.minimum(dx, size - dx), np.minimum(dy, size - dy))
            closer = d < d1
            d2 = np.where(closer, d1, np.minimum(d2, d))
            ids = np.where(closer, k, ids)
            d1 = np.minimum(d1, d)
        return d1, d2, ids

    def save(name, albedo, height, roughness, strength, ao_strength=1.0):
        h = (height - height.min()) / (height.max() - height.min())
        # Normal (OpenGL convention: green = up = towards smaller row numbers).
        dx = (np.roll(h, -1, 1) - np.roll(h, 1, 1)) * 0.5 * strength
        dy = (np.roll(h, -1, 0) - np.roll(h, 1, 0)) * 0.5 * strength
        n = np.stack([-dx, dy, np.ones_like(h)], -1)
        n /= np.linalg.norm(n, axis=-1, keepdims=True)
        # AO: how far below its surroundings a texel lies.
        cavity = np.clip(blur(h, 6.0) - h, 0.0, None)
        ao = np.clip(1.0 - cavity * 4.0 * ao_strength, 0.25, 1.0)
        u8 = lambda a: (np.clip(a, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)
        rgba = np.concatenate([np.clip(albedo, 0, 1), h[..., None]], -1)
        Image.fromarray(u8(rgba), "RGBA").save(out("textures", "ground", name + "_albedo.png"), optimize=True)
        Image.fromarray(u8(n * 0.5 + 0.5), "RGB").save(out("textures", "ground", name + "_normal.png"), optimize=True)
        Image.fromarray(u8(roughness), "L").save(out("textures", "ground", name + "_roughness.png"), optimize=True)
        Image.fromarray(u8(ao), "L").save(out("textures", "ground", name + "_ao.png"), optimize=True)

    def col(c):
        return np.array(c, dtype=np.float64)[None, None, :]

    # Savannah soil: dry red earth, crusted, with pebbles and bits of dead grass.
    base = spectral(1, 1.6, 2)
    fine = spectral(2, 0.6, 40)
    d1, d2, ids = cells(3, 140)
    rng = np.random.default_rng(4)
    sizes = rng.uniform(3.0, 8.0, 140)[ids]
    pebble = np.clip(1.0 - d1 / sizes, 0.0, 1.0) ** 0.6 * (rng.random(140)[ids] < 0.55)
    straws = np.zeros((size, size))
    for k in range(260):
        x0, y0, ang, length = rng.random() * size, rng.random() * size, rng.random() * np.pi, rng.uniform(12, 40)
        t = np.linspace(0.0, 1.0, int(length * 2))
        sx = (x0 + np.cos(ang) * length * t).astype(int) % size
        sy = (y0 + np.sin(ang) * length * t).astype(int) % size
        straws[sy, sx] = np.maximum(straws[sy, sx], rng.uniform(0.5, 1.0))
    straws = np.clip(blur(straws, 0.6) * 3.0, 0, 1)
    height = base * 0.5 + fine * 0.15 + pebble * 0.6 + straws * 0.15
    earth = col((0.47, 0.30, 0.19)) * (0.8 + 0.4 * base[..., None]) * (0.9 + 0.2 * fine[..., None])
    stone = col((0.62, 0.55, 0.47)) * (0.8 + 0.3 * rng.random(140)[ids][..., None])
    albedo = earth * (1 - pebble[..., None]) + stone * pebble[..., None]
    albedo = albedo * (1 - straws[..., None] * 0.8) + col((0.78, 0.66, 0.42)) * straws[..., None] * 0.8
    rough = 0.92 - pebble * 0.2 - straws * 0.1
    save("soil", albedo, height, rough, 18.0)

    # Desert sand: wind ripples (bent by large-scale noise) over fine grain.
    warp = spectral(11, 2.0, 1, 6)
    warp2 = spectral(12, 2.0, 1, 6)
    phase = (yy / size * 24.0 + xx / size * 3.0 + warp * 3.0 + warp2 * 1.5) * 2.0 * np.pi
    ripple = (np.sin(phase) * 0.5 + 0.5) ** 1.6 # sharper crests
    grain = spectral(13, 0.2, 80)
    dunes = spectral(14, 2.2, 1, 4)
    height = ripple * 0.55 + grain * 0.12 + dunes * 0.6
    albedo = col((0.80, 0.62, 0.40)) * (0.85 + 0.25 * ripple[..., None]) * (0.92 + 0.16 * grain[..., None])
    albedo *= 0.9 + 0.2 * dunes[..., None]
    rough = 0.88 + 0.08 * grain
    save("sand", albedo, height, rough, 10.0, 0.5)

    # Jungle floor: layers of fallen leaves on dark soil, a few twigs.
    soil = spectral(21, 1.0, 4)
    height = soil * 0.15
    albedo = col((0.16, 0.11, 0.07)) * (0.8 + 0.4 * soil[..., None])
    rough = 0.8 + 0.1 * soil
    rng = np.random.default_rng(22)
    palette = [(0.33, 0.20, 0.09), (0.45, 0.29, 0.12), (0.26, 0.24, 0.10), (0.20, 0.25, 0.09), (0.52, 0.36, 0.17), (0.30, 0.15, 0.08)]
    for k in range(900):
        cx, cy = rng.random() * size, rng.random() * size
        ang = rng.random() * np.pi
        ln, wd = rng.uniform(14, 34), 0.0
        wd = ln * rng.uniform(0.28, 0.45)
        r = int(ln) + 2
        x0, y0 = int(cx) - r, int(cy) - r
        ys = (np.arange(y0, y0 + 2 * r + 1) % size)
        xs = (np.arange(x0, x0 + 2 * r + 1) % size)
        ly, lx = np.mgrid[y0:y0 + 2 * r + 1, x0:x0 + 2 * r + 1].astype(np.float64)
        u = (lx - cx) * np.cos(ang) + (ly - cy) * np.sin(ang)
        v = -(lx - cx) * np.sin(ang) + (ly - cy) * np.cos(ang)
        # Leaf: pointed ellipse, curled up at the edges, a darker midrib.
        e = (u / ln) ** 2 + (v / (wd * (1.0 - (u / ln) ** 2 * 0.0 + 1e-6))) ** 2
        shape = (np.abs(u) < ln) & (np.abs(v) < wd * np.sqrt(np.clip(1.0 - (u / ln) ** 2, 0, 1)) ** 0.8)
        if not shape.any():
            continue
        c = np.array(palette[rng.integers(len(palette))]) * rng.uniform(0.8, 1.15)
        base_h = height[np.ix_(ys, xs)].max() + 0.03  # on top of what's there
        edge = np.abs(v) / np.maximum(wd, 1e-3)
        leaf_h = base_h + 0.04 * edge
        rib = np.exp(-(v / 1.2) ** 2) * 0.35
        vein = 0.08 * np.sin(u * 0.9 + np.abs(v) * 1.1)
        sub_a = albedo[np.ix_(ys, xs)]
        sub_h = height[np.ix_(ys, xs)]
        sub_r = rough[np.ix_(ys, xs)]
        leaf_col = c[None, None, :] * (1.0 - rib[..., None] - vein[..., None])
        sub_a[shape] = leaf_col[shape]
        sub_h[shape] = np.minimum(leaf_h[shape], 1.0 + base_h)
        sub_r[shape] = rng.uniform(0.55, 0.75)
        albedo[np.ix_(ys, xs)] = sub_a
        height[np.ix_(ys, xs)] = sub_h
        rough[np.ix_(ys, xs)] = sub_r
    height = blur(height, 0.7)
    save("leaves", albedo, height, rough, 30.0, 1.5)

    # Rock: layered, cracked stone.
    big = spectral(31, 2.0, 1)
    mid = spectral(32, 1.2, 4)
    d1, d2, ids = cells(33, 45)
    cracks = np.clip((d2 - d1) / 3.0, 0.0, 1.0)  # 0 at cell borders
    strata = np.sin((yy / size * 7.0 + big * 2.0) * 2.0 * np.pi) * 0.5 + 0.5
    rng = np.random.default_rng(34)
    slab = rng.random(45)[ids]
    height = big * 0.6 + mid * 0.3 + slab * 0.25 + strata * 0.1 - (1.0 - cracks) * 0.35
    albedo = col((0.50, 0.46, 0.41)) * (0.75 + 0.3 * mid[..., None]) * (0.85 + 0.25 * slab[..., None])
    albedo *= (0.92 + 0.12 * strata[..., None])
    albedo = albedo * (0.55 + 0.45 * cracks[..., None])
    rough = 0.72 + 0.15 * mid + 0.1 * (1 - cracks)
    save("rock", albedo, height, rough, 14.0)


if __name__ == "__main__":
    terrain_textures()
    lut_neutral()
    textures()
    reed_screen()
    rondavel()
    audio()
    print("Assets written to", os.path.normpath(ROOT))
