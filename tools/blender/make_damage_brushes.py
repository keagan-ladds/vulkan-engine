"""
Damage brush alphas for Blender (texture paint, vertex paint and sculpt):
greyscale, white = full effect, fading to black before every border so a dab
never shows a square edge. 16-bit PNGs (no banding when sculpting).

    python3 tools/blender/make_damage_brushes.py      # -> tools/blender/brushes/*.png

Edge wear (paint the wear channel / wear mask along edges and corners):
  edge_wear_chips      a band of chipped paint: use as a stroke along an edge
  edge_wear_flakes     scattered small chips: dab or stroke with spacing
  edge_wear_scratches  fine scuffs and scratches, mostly along one direction
Concrete damage (paint / sculpt):
  concrete_spall       a broken-off chunk: jagged outline, stepped fracture inside
  concrete_chunks      a cluster of smaller spalls and chips
  concrete_pitting     air holes and pock marks
  concrete_crack       a branching crack with chipped lips
  concrete_crumble     a crumbling, eroded patch (soft, for breaking edges up)

In Blender: Texture Paint / Sculpt > brush Texture (Image) = the file, Mapping
"View Plane" or "Area Plane" (dabs) or "Stencil"; Falloff "Constant" keeps the
image's own shape. Random angle (Texture > Angle: Random) hides repeats.
Change SEED below for different shapes.
"""
import math
import os

import numpy as np
from PIL import Image, ImageDraw

SIZE = 1024
SEED = 11
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "brushes")

rng = np.random.default_rng(SEED)
yy, xx = (np.mgrid[0:SIZE, 0:SIZE] + 0.5) / SIZE * 2.0 - 1.0  # -1..1, centre 0
radius = np.sqrt(xx ** 2 + yy ** 2)


def noise(beta=2.0, lo=1.0):
    """Fractal noise (1/f^beta spectrum), 0..1."""
    k = np.fft.fftfreq(SIZE) * SIZE
    kx, ky = np.meshgrid(k, k)
    r = np.sqrt(kx ** 2 + ky ** 2)
    amp = np.where(r >= lo, np.maximum(r, 1e-6) ** (-beta / 2.0 - 0.5), 0.0)
    v = np.real(np.fft.ifft2(amp * np.exp(2j * np.pi * rng.random((SIZE, SIZE)))))
    return (v - v.min()) / (v.max() - v.min())


def smoothstep(e0, e1, v):
    t = np.clip((v - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def warp(img, strength, beta=2.2):
    dx = (noise(beta) - 0.5) * strength * SIZE
    dy = (noise(beta) - 0.5) * strength * SIZE
    ys, xs = np.mgrid[0:SIZE, 0:SIZE].astype(np.float64)
    sx = np.clip(xs + dx, 0, SIZE - 1.001)
    sy = np.clip(ys + dy, 0, SIZE - 1.001)
    x0, y0 = sx.astype(int), sy.astype(int)
    tx, ty = sx - x0, sy - y0
    a = img[y0, x0] * (1 - tx) + img[y0, x0 + 1] * tx
    b = img[y0 + 1, x0] * (1 - tx) + img[y0 + 1, x0 + 1] * tx
    return a * (1 - ty) + b * ty


def window(inner=0.7, outer=0.95):
    """1 in the middle, 0 from `outer` (of the half-size) outwards: no square edges."""
    return 1.0 - smoothstep(inner, outer, radius)


def blur(v, r):
    """Gaussian blur, radius (sigma) `r` pixels (FFT; the image is black at its borders anyway)."""
    k = np.fft.fftfreq(SIZE)
    kx, ky = np.meshgrid(k, k)
    return np.real(np.fft.ifft2(np.fft.fft2(v) * np.exp(-2.0 * (math.pi * r) ** 2 * (kx ** 2 + ky ** 2))))


def save(name, v):
    v = np.clip(v, 0.0, 1.0)
    os.makedirs(OUT, exist_ok=True)
    Image.fromarray((v * 65535.0 + 0.5).astype(np.uint16)).save(os.path.join(OUT, name + ".png"))


def canvas_lines(segments, scale=4):
    """Draws [(points, width, value)] polylines anti-aliased (supersampled)."""
    n = SIZE * scale
    img = Image.new("F", (n, n), 0.0)
    d = ImageDraw.Draw(img)
    for pts, width, value in segments:
        d.line([(x * scale, y * scale) for x, y in pts], fill=value, width=max(1, int(width * scale)), joint="curve")
    img = img.resize((SIZE, SIZE), Image.BOX)
    return np.asarray(img, dtype=np.float64)


# --- Edge wear ---------------------------------------------------------------

def edge_wear_chips():
    # Chipped paint concentrated on a horizontal band (the edge), thinning out away from it.
    edge = (noise(1.8, lo=2) - 0.5) * 0.25  # the band wanders a little
    band = np.exp(-((yy - edge) / 0.3) ** 2)
    field = warp(noise(1.5, lo=5), 0.03) * 0.7 + noise(1.0, lo=28) * 0.3
    density = band * (1.0 - smoothstep(0.8, 0.95, np.abs(xx)))
    chips = smoothstep(0.0, 0.02, field - (1.0 - 0.62 * density))
    chips *= 0.85 + 0.15 * noise(1.5, lo=8)
    save("edge_wear_chips", chips * window(0.85, 0.97))


def edge_wear_flakes():
    field = warp(noise(1.4, lo=10), 0.02) * 0.8 + noise(0.8, lo=40) * 0.2
    t = np.quantile(field, 0.86)
    flakes = smoothstep(t, t + 0.01, field)
    flakes *= smoothstep(0.0, 0.4, noise(2.0, lo=2))  # clusters, not an even sprinkle
    save("edge_wear_flakes", flakes * window(0.55, 0.9))


def edge_wear_scratches():
    segs = []
    base_angle = rng.uniform(-0.3, 0.3)
    for _ in range(260):
        a = base_angle + rng.normal(0, 0.18) + (math.pi / 2 if rng.random() < 0.08 else 0.0)
        length = SIZE * rng.uniform(0.04, 0.35) * (rng.random() ** 1.5 + 0.2)
        cx, cy = rng.normal(SIZE / 2, SIZE * 0.18, 2)
        pts, x, y = [], cx - math.cos(a) * length / 2, cy - math.sin(a) * length / 2
        steps = 12
        for i in range(steps + 1):
            pts.append((x, y))
            a += rng.normal(0, 0.03)
            x += math.cos(a) * length / steps
            y += math.sin(a) * length / steps
        segs.append((pts, rng.uniform(0.6, 2.6), rng.uniform(0.35, 1.0)))
    lines = canvas_lines(segs)
    lines *= 0.6 + 0.4 * noise(1.2, lo=6)  # scratches fade in and out along their length
    save("edge_wear_scratches", np.clip(lines, 0, 1) * window(0.6, 0.95))


# --- Concrete ----------------------------------------------------------------

def spall_shape(cx, cy, size, jag=0.6):
    """A broken-off chunk: jagged, uneven outline, stepped (terraced) fracture depth."""
    a = rng.uniform(0, math.pi)
    stretch = rng.uniform(1.2, 1.7)
    u = ((xx - cx) * math.cos(a) + (yy - cy) * math.sin(a)) / (size * stretch)
    v = (-(xx - cx) * math.sin(a) + (yy - cy) * math.cos(a)) / (size / stretch ** 0.5)
    d = np.sqrt(u ** 2 + v ** 2)
    edge = warp(d, 0.05) + (noise(1.3, lo=2) - 0.5) * jag + (noise(0.9, lo=18) - 0.5) * jag * 0.4
    inside = smoothstep(1.0, 0.96, edge)
    depth = np.clip(1.0 - edge, 0.0, 1.0) ** 0.6
    terraces = np.floor(depth * 4 + noise(1.8, lo=4) * 1.5) / 5.0  # fracture planes
    rough = noise(0.9, lo=30)
    return inside * np.clip(0.35 + 0.45 * terraces + 0.2 * rough, 0.0, 1.0)


def concrete_spall():
    save("concrete_spall", spall_shape(0.0, 0.0, 0.62) * window(0.85, 0.98))


def concrete_chunks():
    v = np.zeros((SIZE, SIZE))
    for _ in range(9):
        r = rng.uniform(0.06, 0.2)
        ang, dist = rng.uniform(0, 2 * math.pi), rng.uniform(0.1, 0.62 - r)
        v = np.maximum(v, spall_shape(math.cos(ang) * dist, math.sin(ang) * dist, r, jag=0.5))
    save("concrete_chunks", v * window(0.8, 0.97))


def concrete_pitting():
    v = np.zeros((SIZE, SIZE))
    for _ in range(420):
        r = 0.006 + 0.035 * rng.random() ** 3  # many small, few large
        cx, cy = rng.normal(0, 0.33, 2)
        d = np.sqrt((xx - cx) ** 2 + (yy - cy) ** 2) / r
        pit = np.sqrt(np.clip(1.0 - d ** 2, 0.0, 1.0))  # hemispherical air hole
        v = np.maximum(v, pit * rng.uniform(0.5, 1.0))
    v *= smoothstep(0.1, 0.5, noise(2.0, lo=2))
    save("concrete_pitting", v * window(0.6, 0.92))


def concrete_crack():
    segs = []

    def branch(x, y, angle, length, width, depth):
        steps = max(4, int(length / 10))
        pts = [(x, y)]
        for i in range(steps):
            angle += rng.normal(0, 0.18)
            x += math.cos(angle) * length / steps
            y += math.sin(angle) * length / steps
            pts.append((x, y))
            w = width * (1.0 - i / steps * 0.8)
            if depth < 3 and rng.random() < 0.12:
                branch(x, y, angle + rng.choice([-1, 1]) * rng.uniform(0.5, 1.1), length * rng.uniform(0.3, 0.55), w * 0.6, depth + 1)
        for a, b in zip(pts, pts[1:]):
            k = pts.index(a) / steps
            segs.append(([a, b], max(0.8, width * (1.0 - k * 0.8)), 1.0))

    c = SIZE / 2
    a0 = rng.uniform(0, math.pi)
    branch(c, c, a0, SIZE * 0.42, 7.0, 0)
    branch(c, c, a0 + math.pi, SIZE * 0.36, 6.0, 0)
    crack = np.clip(canvas_lines(segs), 0, 1)
    # Chipped lips: the crack widened where noise says so.
    lips = blur(crack, 6) * 3.0
    chipped = smoothstep(0.55, 0.6, lips * (0.6 + 0.8 * noise(1.2, lo=12))) * 0.55
    save("concrete_crack", np.maximum(crack, chipped) * window(0.75, 0.97))


def concrete_crumble():
    field = warp(noise(1.9, lo=2), 0.06) * 0.7 + noise(1.1, lo=16) * 0.3
    shape = smoothstep(0.38, 0.62, field + (0.35 - radius * 0.7))
    grit = 0.55 + 0.45 * noise(0.8, lo=40)
    save("concrete_crumble", np.clip(shape * grit, 0, 1) * window(0.65, 0.95))


BRUSHES = [edge_wear_chips, edge_wear_flakes, edge_wear_scratches,
           concrete_spall, concrete_chunks, concrete_pitting, concrete_crack, concrete_crumble]

if __name__ == "__main__":
    for make in BRUSHES:
        make()
        print("wrote", os.path.join(OUT, make.__name__ + ".png"))
