"""Summarizes tools/perf/shader_variants.sh logs: medians per variant / view, and the
three questions from docs/08-perf-tests.md with a verdict each.

    python3 tools/perf/summarize.py <output dir>
"""
import collections
import glob
import os
import re
import statistics
import sys

ANSI = re.compile(r"\x1b\[[0-9;]*m")
out = sys.argv[1]
runs = collections.defaultdict(list)  # (label, scene) -> [ {metric: ms} ]
info = {}
for path in sorted(glob.glob(os.path.join(out, "logs", "*.log"))):
    label, scene, _rep, _ = os.path.basename(path).split(".")
    m = {}
    for line in open(path, errors="replace"):
        line = ANSI.sub("", line)
        if g := re.search(r"\[bench\] \d+ frames: .*frame ([0-9.]+) ms.* gpu ([0-9.]+) ms", line):
            m["frame"], m["gpu"] = float(g[1]), float(g[2])
        elif g := re.search(r"\[bench\]   (.+?)\s+([0-9.]+) ms", line):
            m[g[1].strip()] = float(g[2])
        elif g := re.search(r"\bGPU: (.+)", line):
            info.setdefault("device", g[0].strip())
    if "gpu" in m:
        runs[(label, scene)].append(m)
    else:
        print(f"<!-- no [bench] result in {os.path.basename(path)} (crashed? see the log) -->")


def med(label, scene, metric):
    v = [r[metric] for r in runs.get((label, scene), []) if metric in r]
    return (statistics.median(v), (max(v) - min(v)) / statistics.median(v) if len(v) > 1 and statistics.median(v) else 0.0) if v else (None, None)


scenes = sorted({s for _, s in runs}, key=lambda s: ["camp-sun", "jungle", "vista", "camp-noleaves"].index(s) if s in ["camp-sun", "jungle", "vista", "camp-noleaves"] else 9)
labels = ["base-on", "base-off", "notrans", "nodiscard"]
print("# Standard shader variants: results\n")
if "device" in info:
    print(f"Device line from the log: `{info['device']}`\n")
print("Medians over the repeats, GPU milliseconds per frame (the spread is (max-min)/median).\n")
print("| view | variant | GPU total | Opaque | Shadows | spread (Opaque) |")
print("|---|---|---:|---:|---:|---:|")
for s in scenes:
    for l in labels:
        if (l, s) not in runs:
            continue
        g, _ = med(l, s, "gpu"); o, so = med(l, s, "Opaque"); sh, _ = med(l, s, "Shadows")
        print(f"| {s} | {l} | {g:.3f} | {o:.3f} | {sh:.3f} | {so * 100:.1f}% |")


def compare(title, a, b, scene_list, meaning_yes, meaning_no):
    print(f"\n## {title}\n")
    print(f"`{b}` relative to `{a}`, Opaque pass:\n")
    yes = False
    for s in scene_list:
        (va, na), (vb, nb) = med(a, s, "Opaque"), med(b, s, "Opaque")
        if va is None or vb is None:
            continue
        d = (vb - va) / va
        noise = max(na or 0, nb or 0)
        threshold = max(0.02, 2 * noise)
        sig = abs(d) > threshold
        yes |= sig and d < 0
        print(f"- {s}: {va:.3f} -> {vb:.3f} ms ({d * 100:+.1f}%; significant beyond {threshold * 100:.1f}%: {'yes' if sig else 'no'})")
    print(f"\n**Verdict:** {meaning_yes if yes else meaning_no}")


leafy = [s for s in scenes if s != "camp-noleaves"]
compare("Q1. What does translucency cost when it is on?", "base-on", "base-off", leafy,
        "turning it off saves a measurable amount: keep it optional, and consider a cheaper version (no second shadow lookup) for low settings.",
        "the cost when on is within noise: nothing to do.")
compare("Q2. Do materials that don't use translucency pay for its code? (register pressure)", "base-off", "notrans", scenes,
        "yes, compiling it out is faster: make translucency a specialization constant / separate pipeline.",
        "no, the runtime branch is free enough: leave it as a branch.")
compare("Q3. Does `discard` in the shared shader slow opaque materials? (early depth test)", "base-off", "nodiscard", ["camp-noleaves"],
        "yes: give opaque materials a shader without discard (cutout keeps its own).",
        "no measurable effect on this GPU: the split isn't worth it yet.")
