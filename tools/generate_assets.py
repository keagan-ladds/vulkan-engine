#!/usr/bin/env python3
"""
Generates the sandbox's test assets so the repository needs no binary
downloads (and so the engine's importers get exercised on real files):

  sandbox/assets/textures/ground_detail.png   tileable grey noise (terrain detail)
  sandbox/assets/textures/mud_wall.png        red-clay wall texture
  sandbox/assets/textures/thatch.png          straw roof texture
  sandbox/assets/models/rondavel.gltf (+.bin) round African hut: walls + thatched
                                              roof (one mesh, two materials) + door node
  sandbox/assets/audio/*.wav                  wind / fire / rain loops, pickup chime

Requires Pillow (python3-pil). Run from anywhere:
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


if __name__ == "__main__":
    textures()
    rondavel()
    audio()
    print("Assets written to", os.path.normpath(ROOT))
