#!/usr/bin/env python3
"""Checks the engine's module layering (docs/01-architecture.md section 2).

Scans every file under engine/src/ve/<module>/ for `#include "ve/<other>/..."`
(or <ve/<other>/...>) and reports includes of a module that <module> may not
depend on. Exit code 1 if there are any.

    python3 tools/check_layering.py

The rule: core <- gfx <- render <- scene; the feature modules (physics, anim,
gameplay, nav, net, world) build on the scene; assets, audio, ui and debug sit
on top; the editor may use everything. A module never reaches sideways or
upwards, except for the known exceptions listed below (each with its reason).
"""

import pathlib
import re
import sys

# Each module's allowed dependencies (besides itself). Keep this in step with
# docs/01-architecture.md; a new module must be added here.
ALLOWED = {
    "core": set(),
    "gfx": {"core"},
    "render": {"core", "gfx"},
    "scene": {"core", "gfx", "render"},
    "physics": {"core", "render", "scene"},
    "anim": {"core", "render", "scene", "physics"},
    "gameplay": {"core", "render", "scene", "physics"},
    "net": {"core", "scene"},
    "world": {"core", "gfx", "render", "scene", "assets", "physics"},
    "nav": {"core", "render", "scene", "physics", "world"},
    "assets": {"core", "gfx", "render", "scene", "anim", "physics", "audio"},
    "audio": {"core", "scene", "assets", "physics"},
    "debug": {"core", "gfx", "render", "scene", "physics", "nav"},
    "ui": {"core", "gfx", "render", "scene", "assets", "audio", "physics", "debug"},
    "editor": "*",  # the editor may depend on everything
}

# Files that aren't part of any module's layering.
EXEMPT = {
    "core/Engine.hpp",  # the composition root: creates and wires every module
    "core/Engine.cpp",
    "ve.hpp",  # the umbrella header games include
}

# Known exceptions: (file relative to engine/src/ve, module it may include).
EXCEPTIONS = {
    # The renderer reads scene components (MeshRenderer, lights, Camera) as
    # plain data each frame; the doc's "the renderer reads the scene".
    ("render/SceneGatherer.cpp", "scene"),
    # ModelInstance loads a model by path and sets its Animator's clip.
    ("scene/MeshComponents.cpp", "assets"),
    ("scene/MeshComponents.cpp", "anim"),
    # Prefab / scene files resolve asset paths through the AssetManager.
    ("scene/Prefab.cpp", "assets"),
    ("scene/SceneSerializer.cpp", "assets"),
    # Save games record and restore dynamic Rigidbodies.
    ("scene/SaveGame.cpp", "physics"),
    # ve::debug drawing (collider and navmesh debug lines); in turn
    # debug::drawEngineChannels draws them (a .cpp-level cycle only).
    # (Physics: only its debug-drawing file.)
    ("physics/PhysicsDebug.cpp", "debug"),
    ("nav/Navigation.cpp", "debug"),
    # Ambient layers weigh themselves by the terrain biomes / foliage around the listener.
    ("audio/Ambience.cpp", "world"),
    # Foliage: building-piece ghosts don't clear foliage (PlacementGhost).
    ("world/Vegetation.cpp", "gameplay"),
    # Its F1-overlay panel only shows while the overlay is open (UiLayer::debugOverlay).
    ("world/Foliage.cpp", "ui"),
}

INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]ve/([A-Za-z_]+)/')


def main() -> int:
    root = pathlib.Path(__file__).resolve().parent.parent / "engine" / "src" / "ve"
    violations, used = [], set()
    for path in sorted(root.rglob("*")):
        if path.suffix not in (".hpp", ".cpp", ".h", ".inl") or not path.is_file():
            continue
        rel = path.relative_to(root).as_posix()
        if rel in EXEMPT:
            continue
        module = rel.split("/")[0]
        if module not in ALLOWED:
            violations.append(f"{rel}: module '{module}' is not in ALLOWED (add it to tools/check_layering.py)")
            continue
        allowed = ALLOWED[module]
        for n, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            m = INCLUDE.match(line)
            if not m:
                continue
            other = m.group(1)
            if other == module or allowed == "*" or other in allowed:
                continue
            if (rel, other) in EXCEPTIONS:
                used.add((rel, other))
                continue
            violations.append(f"{rel}:{n}: {module} -> {other}: {line.strip()}")
    for rel, other in sorted(EXCEPTIONS - used):
        print(f"note: exception no longer needed: ({rel!r}, {other!r})")
    for v in violations:
        print(v)
    if violations:
        print(f"{len(violations)} layering violation(s)")
        return 1
    print("layering OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
