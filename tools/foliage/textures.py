"""Procedural textures for the foliage generator (numpy + Pillow, no downloads).

Leaf cards: RGBA atlases of leaf sprays drawn as vector shapes (supersampled),
with the colour bled past the alpha edge so mipmaps and alpha-tested
filtering don't pick up dark fringes.

Bark: tileable albedo + normal maps from periodic spectral noise (FFT), so the
textures wrap seamlessly around trunks and along them.

    python3 tools/foliage/textures.py <out dir>      # writes every texture

The tree generator reads the atlas layouts from ATLASES (which cell holds
which card, and where its attachment point is).
"""

import math
import os
import random
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

SS = 3  # supersampling for the vector drawing


# -----------------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------------

def spectral_noise(size, beta, seed, aspect=(1.0, 1.0)):
    """Tileable noise with a 1/f^beta spectrum; aspect stretches it (x, y)."""
    rng = np.random.default_rng(seed)
    fx = np.fft.fftfreq(size)[None, :] * aspect[0]
    fy = np.fft.fftfreq(size)[:, None] * aspect[1]
    f = np.sqrt(fx * fx + fy * fy)
    f[0, 0] = 1.0
    amp = 1.0 / np.power(f, beta)
    amp[0, 0] = 0.0
    phase = rng.uniform(0, 2 * np.pi, (size, size))
    n = np.real(np.fft.ifft2(amp * np.exp(1j * phase)))
    n -= n.min()
    return n / max(n.max(), 1e-9)


def normal_map(height, strength):
    """Tangent-space normal map (OpenGL / glTF convention: +Y up) from a tileable height field."""
    dx = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * 0.5
    dy = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) * 0.5
    n = np.stack([-dx * strength, dy * strength, np.ones_like(height)], axis=-1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    return Image.fromarray(((n * 0.5 + 0.5) * 255).astype(np.uint8), "RGB")


def colorize(t, stops):
    """t in 0..1 -> RGB through colour stops [(pos, (r, g, b)), ...]."""
    out = np.zeros(t.shape + (3,), np.float32)
    for c in range(3):
        out[..., c] = np.interp(t, [p for p, _ in stops], [col[c] for _, col in stops])
    return out


def bleed(img, iterations=16):
    """Spread colour into transparent pixels (alpha unchanged): no dark halos when filtered / mipmapped."""
    a = np.asarray(img).astype(np.float32)
    rgb, alpha = a[..., :3].copy(), a[..., 3]
    known = alpha > 8.0
    for _ in range(iterations):
        acc = np.zeros_like(rgb)
        cnt = np.zeros(alpha.shape, np.float32)
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                k = np.roll(np.roll(known, dy, 0), dx, 1)
                acc += np.roll(np.roll(rgb, dy, 0), dx, 1) * k[..., None]
                cnt += k
        grow = (~known) & (cnt > 0)
        rgb[grow] = acc[grow] / cnt[grow][:, None]
        known = known | grow
    # Whatever is still unknown: the average leaf colour.
    if (~known).any():
        rgb[~known] = rgb[known].mean(axis=0) if known.any() else 0
    return Image.fromarray(np.concatenate([np.clip(rgb, 0, 255), alpha[..., None]], -1).astype(np.uint8), "RGBA")


def jitter(c, rnd, amount):
    return tuple(int(max(0, min(255, v + rnd.uniform(-amount, amount)))) for v in c)


def mix(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


# -----------------------------------------------------------------------------
# Leaf shapes (drawn into a supersampled RGBA canvas)
# -----------------------------------------------------------------------------

def ellipse_leaf(draw, x, y, length, width, angle, color, rnd, midrib=None):
    """An ovate leaf as a polygon from its base at (x, y) along `angle`."""
    pts = []
    n = 14 if length > 12 * SS else 5
    ca, sa = math.cos(angle), math.sin(angle)
    for i in range(n + 1):  # one side, base -> tip
        t = i / n
        w = width * math.sin(math.pi * t) ** 0.8 * (1.0 - 0.25 * t)
        pts.append((t * length, w))
    pts += [(px, -py) for px, py in reversed(pts[1:-1])]
    poly = [(x + px * ca - py * sa, y + px * sa + py * ca) for px, py in pts]
    draw.polygon(poly, fill=color + (255,))
    if midrib and width > 2.5 * SS:
        draw.line([(x, y), (x + length * 0.9 * ca, y + length * 0.9 * sa)], fill=midrib + (255,), width=max(1, int(width * 0.12)))


def bipinnate_leaf(draw, x, y, angle, length, rnd, base, light, shade):
    """An acacia leaf: a rachis with pairs of pinnae, each a row of tiny paired leaflets."""
    ca, sa = math.cos(angle), math.sin(angle)
    draw.line([(x, y), (x + length * ca, y + length * sa)], fill=mix(base, (90, 80, 50), 0.5) + (255,), width=max(1, SS // 2))
    pairs = rnd.randint(3, 6)
    col = jitter(mix(mix(base, light, rnd.random() * 0.7), (20, 26, 12), shade), rnd, 8)
    for p in range(pairs):
        t = 0.2 + 0.8 * (p + 0.5) / pairs
        px, py = x + length * t * ca, y + length * t * sa
        plen = length * rnd.uniform(0.3, 0.42) * (1.15 - 0.45 * t)
        for side in (-1, 1):
            a = angle + side * rnd.uniform(0.95, 1.4)
            qa, qs = math.cos(a), math.sin(a)
            leaflets = rnd.randint(7, 11)
            for k in range(leaflets):
                u = (k + 0.6) / leaflets
                lx, ly = px + plen * u * qa, py + plen * u * qs
                for s2 in (-1, 1):
                    ellipse_leaf(draw, lx, ly, SS * rnd.uniform(2.6, 3.6) * (1.15 - 0.4 * u), SS * rnd.uniform(0.9, 1.25), a + s2 * 1.3,
                                 jitter(col, rnd, 10), rnd)


def thin_twig(draw, x, y, angle, length, width, color):
    draw.line([(x, y), (x + length * math.cos(angle), y + length * math.sin(angle))], fill=color + (255,), width=max(1, int(width)))


# -----------------------------------------------------------------------------
# Card painters: each draws one card into a (w, h) cell. The card's
# attachment point (where it meets its twig) is at the bottom centre.
# -----------------------------------------------------------------------------

def card_acacia(w, h, seed, base=(70, 90, 38), light=(124, 140, 60)):
    """A dense, feathery spray of acacia foliage: zig-zag twigs with tufts of
    bipinnate leaves at every node (Vachellia tortilis), filling a fan with gaps."""
    rnd = random.Random(seed)
    img = Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx, cy = w * SS * 0.5, h * SS * 0.97
    twig = (78, 62, 48)
    nodes = []
    def grow(x, y, a, length, width, depth):
        steps = max(2, int(length / (SS * 16)))
        for i in range(steps):  # zig-zag: a slight turn at every node
            a += rnd.uniform(-0.28, 0.28)
            seg = length / steps
            nx, ny = x + seg * math.cos(a), y + seg * math.sin(a)
            d.line([(x, y), (nx, ny)], fill=twig + (255,), width=max(1, int(width)))
            nodes.append((nx, ny, a, depth))
            if depth < 2 and rnd.random() < 0.45:
                grow(nx, ny, a + rnd.choice((-1, 1)) * rnd.uniform(0.4, 0.8), length * 0.55, width * 0.7, depth + 1)
            x, y = nx, ny
            width *= 0.85
    for i in range(rnd.randint(3, 4)):
        grow(cx, cy, -math.pi / 2 + rnd.uniform(-0.7, 0.7), h * SS * rnd.uniform(0.36, 0.48), SS * 3.0, 0)
    # Tufts: lower / inner ones first and darker (self-shadowing), outer ones last and lighter.
    tufts = []
    for (x, y, a, depth) in nodes:
        for _ in range(rnd.randint(2, 4)):
            tufts.append((x, y, a + rnd.uniform(-1.6, 1.6), depth))
    rnd.shuffle(tufts)
    tufts.sort(key=lambda t: -t[1])  # bottom first
    margin = w * SS * 0.1
    tufts = [t for t in tufts if margin < t[0] < w * SS - margin and margin < t[1]]
    for i, (x, y, a, depth) in enumerate(tufts):
        shade = 0.35 * (1.0 - i / max(1, len(tufts) - 1))
        bipinnate_leaf(d, x, y, a, SS * rnd.uniform(22, 36) * (w / 512.0) * 1.0, rnd, base, light, shade)
    # Paired white thorns at some nodes.
    for (x, y, a, depth) in rnd.sample(nodes, min(len(nodes), 10)):
        for side in (-1, 1):
            thin_twig(d, x, y, a + side * 1.2, SS * rnd.uniform(5, 9), SS * 0.8, (226, 222, 206))
    return img.resize((w, h), Image.LANCZOS)


def card_broadleaf(w, h, seed, base=(46, 92, 38), light=(96, 150, 60), size=(26, 46), count=(34, 50)):
    """A cluster of ovate leaves with midribs (jungle canopy, shrubs)."""
    rnd = random.Random(seed)
    img = Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx, cy = w * SS * 0.5, h * SS * 0.96
    twig = (70, 56, 40)
    stems = []
    for i in range(rnd.randint(4, 6)):
        a = -math.pi / 2 + rnd.uniform(-0.85, 0.85)
        L = h * SS * rnd.uniform(0.5, 0.85)
        thin_twig(d, cx, cy, a, L, SS * 2.5, twig)
        stems.append((a, L))
    leaves = []
    for _ in range(rnd.randint(*count)):  # along the stems, each on a short stalk to one side
        sa, sl = stems[rnd.randrange(len(stems))]
        r = sl * rnd.uniform(0.25, 1.0)
        bx, by = cx + r * math.cos(sa), cy + r * math.sin(sa)
        side = rnd.choice((-1, 1))
        off = h * SS * rnd.uniform(0.0, 0.07)
        x, y = bx - side * math.sin(sa) * off, by + side * math.cos(sa) * off
        if off > SS:
            thin_twig(d, bx, by, math.atan2(y - by, x - bx), off, SS * 1.2, twig)
        leaves.append((x, y, sa + side * rnd.uniform(0.4, 1.3), r / (h * SS)))
    leaves.sort(key=lambda l: l[3])  # far (old) leaves first
    for (x, y, a, t) in leaves:
        col = jitter(mix(base, light, rnd.random() * 0.7 * t + 0.1), rnd, 10)
        L = SS * rnd.uniform(*size) * (w / 256.0)
        ellipse_leaf(d, x, y, L, L * rnd.uniform(0.32, 0.42), a, col, rnd, midrib=mix(col, (200, 210, 150), 0.35))
    return img.resize((w, h), Image.LANCZOS)


def card_palmate(w, h, seed, base=(70, 104, 46), light=(120, 150, 70)):
    """Baobab-like digitate leaves: 5-7 leaflets from one point, a few per card."""
    rnd = random.Random(seed)
    img = Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx, cy = w * SS * 0.5, h * SS * 0.96
    for _ in range(rnd.randint(15, 20)):
        a = -math.pi / 2 + rnd.uniform(-0.9, 0.9)
        L = h * SS * rnd.uniform(0.25, 0.62)
        thin_twig(d, cx, cy, a, L, SS * 2, (96, 80, 60))
        hx, hy = cx + L * math.cos(a), cy + L * math.sin(a)
        n = rnd.randint(5, 7)
        col = jitter(mix(base, light, rnd.random() * 0.6), rnd, 10)
        for k in range(n):
            la = a + (k - (n - 1) / 2) * 0.42
            ellipse_leaf(d, hx, hy, SS * rnd.uniform(24, 34) * (w / 256.0), SS * rnd.uniform(6.5, 8.5) * (w / 256.0), la, jitter(col, rnd, 8), rnd, midrib=mix(col, (190, 200, 150), 0.3))
    return img.resize((w, h), Image.LANCZOS)


def card_frond(w, h, seed, base=(58, 104, 40), light=(110, 150, 62), droop=0.0, leaflet=(70, 12), pinnae=34):
    """A pinnate frond (palm leaf / fern), rachis along the card's height, base at the bottom."""
    rnd = random.Random(seed)
    img = Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    x0, y0 = w * SS * 0.5, h * SS * 0.99
    length = h * SS * 0.95
    pts = []
    for i in range(41):
        t = i / 40
        pts.append((x0 + droop * w * SS * 0.25 * t * t, y0 - length * t))
    d.line(pts, fill=(120, 110, 60, 255), width=max(1, SS * 3))
    for k in range(pinnae):
        t = 0.08 + 0.9 * k / pinnae
        i = min(int(t * 40), 39)
        (px, py), (qx, qy) = pts[i], pts[i + 1]
        a = math.atan2(qy - py, qx - px)
        size = math.sin(math.pi * (0.15 + 0.85 * t)) ** 0.7
        for side in (-1, 1):
            la = a + side * rnd.uniform(0.85, 1.15)
            col = jitter(mix(base, light, rnd.random() * 0.6), rnd, 10)
            L = SS * leaflet[0] * size * (w / 256.0) * rnd.uniform(0.85, 1.1)
            ellipse_leaf(d, px, py, L, SS * leaflet[1] * size * (w / 256.0) * 0.5 + SS, la, col, rnd)
    return img.resize((w, h), Image.LANCZOS)


# The atlases: (file, cell size, grid, [(painter, seed, kwargs)...]); the
# generator maps cards to cells in this order.
ATLASES = {
    "acacia_leaves": (512, 2, [(card_acacia, s, {}) for s in (11, 12, 13, 14)]),
    "fever_leaves": (512, 2, [(card_acacia, s, {"base": (96, 120, 46), "light": (156, 170, 70)}) for s in (21, 22, 23, 24)]),
    "baobab_leaves": (512, 2, [(card_palmate, s, {}) for s in (31, 32, 33, 34)]),
    "bush_leaves": (512, 2, [(card_broadleaf, s, {"base": (70, 92, 44), "light": (120, 132, 64), "size": (14, 24), "count": (70, 95)})
                             for s in (41, 42, 43, 44)]),
    "jungle_leaves": (512, 2, [(card_broadleaf, s, {}) for s in (51, 52, 53, 54)]),
    "palm_frond": (512, 2, [(card_frond, s, {"droop": 0.0, "leaflet": (110, 10), "pinnae": 40}) for s in (61, 62, 63, 64)]),
    "fern_frond": (512, 2, [(card_frond, s, {"base": (48, 98, 36), "light": (100, 150, 60), "leaflet": (60, 16), "pinnae": 26})
                            for s in (71, 72, 73, 74)]),
}


def build_atlas(name, out_dir):
    cell, grid, cards = ATLASES[name]
    atlas = Image.new("RGBA", (cell * grid, cell * grid), (0, 0, 0, 0))
    for i, (painter, seed, kw) in enumerate(cards):
        img = painter(cell, cell, seed, **kw)
        atlas.paste(img, ((i % grid) * cell, (i // grid) * cell))
    atlas = bleed(atlas)
    atlas.save(os.path.join(out_dir, name + ".png"))
    return atlas


# -----------------------------------------------------------------------------
# Bark (tileable)
# -----------------------------------------------------------------------------

def bark(name, out_dir, size=512, seed=1, kind="fissured"):
    if kind == "fissured":      # acacia: dark grey-brown, deep vertical fissures
        ridges = spectral_noise(size, 1.6, seed, aspect=(0.18, 1.0))  # fast across x: vertical fissures
        detail = spectral_noise(size, 1.2, seed + 1)
        h = np.clip(ridges * 1.25 - 0.15, 0, 1) ** 1.6 * 0.8 + detail * 0.2
        col = colorize(h, [(0.0, (34, 28, 24)), (0.45, (72, 60, 50)), (0.8, (104, 92, 78)), (1.0, (128, 118, 102))])
        strength = 9.0
    elif kind == "fever":       # fever tree: yellow-green, powdery, fine horizontal lenticels
        base = spectral_noise(size, 2.0, seed)
        lent = spectral_noise(size, 1.0, seed + 1, aspect=(1.0, 0.12))  # horizontal lenticels
        h = base * 0.6 + np.clip(lent * 1.3 - 0.75, 0, 1) * 0.4
        col = colorize(base, [(0.0, (146, 150, 70)), (0.6, (186, 184, 92)), (1.0, (210, 204, 130))])
        col *= (1.0 - np.clip(lent * 1.3 - 0.75, 0, 1)[..., None] * 0.45)
        strength = 4.0
    elif kind == "baobab":      # smooth grey with horizontal folds
        folds = spectral_noise(size, 1.8, seed, aspect=(1.0, 0.15))  # horizontal folds
        blot = spectral_noise(size, 2.2, seed + 1)
        fine = spectral_noise(size, 1.1, seed + 3)
        h = folds * 0.45 + blot * 0.3 + fine * 0.25
        col = colorize(blot * 0.6 + fine * 0.4, [(0.0, (116, 104, 96)), (0.5, (146, 134, 122)), (1.0, (170, 158, 144))])
        col *= (0.92 + 0.08 * folds)[..., None]
        strength = 5.0
    elif kind == "palm":        # ringed leaf scars
        rings = np.sin(np.linspace(0, 2 * np.pi * 14, size, endpoint=False))[:, None] * np.ones((1, size))
        fib = spectral_noise(size, 1.3, seed, aspect=(0.25, 1.0))
        h = np.clip(rings * 0.5 + 0.5, 0, 1) ** 2 * 0.6 + fib * 0.4
        col = colorize(h, [(0.0, (70, 58, 44)), (0.5, (112, 96, 74)), (1.0, (150, 134, 108))])
        strength = 7.0
    else:                       # jungle: grey-brown with lichen
        base = spectral_noise(size, 1.5, seed, aspect=(0.35, 1.0))
        lichen = spectral_noise(size, 1.5, seed + 2)
        h = base
        col = colorize(base, [(0.0, (62, 54, 46)), (0.6, (100, 90, 78)), (1.0, (128, 118, 102))])
        m = (np.clip((lichen - 0.7) * 5, 0, 1) * 0.55)[..., None]  # small, faint patches
        col = col * (1 - m) + np.array([132, 140, 108], np.float32) * m
        strength = 6.0
    Image.fromarray(np.clip(col, 0, 255).astype(np.uint8), "RGB").save(os.path.join(out_dir, name + "_albedo.png"))
    normal_map(h, strength).save(os.path.join(out_dir, name + "_normal.png"))


BARKS = {"acacia_bark": "fissured", "fever_bark": "fever", "baobab_bark": "baobab", "palm_bark": "palm", "jungle_bark": "jungle"}


def build_all(out_dir):
    os.makedirs(out_dir, exist_ok=True)
    for i, (name, kind) in enumerate(BARKS.items()):
        bark(name, out_dir, seed=100 + i * 7, kind=kind)
    for name in ATLASES:
        build_atlas(name, out_dir)


if __name__ == "__main__":
    build_all(sys.argv[1] if len(sys.argv) > 1 else "sandbox/assets/textures/foliage")
