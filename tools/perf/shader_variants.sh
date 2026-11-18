#!/bin/bash
# Performance test for the standard shader's optional features (see docs/08-perf-tests.md).
#
#   tools/perf/shader_variants.sh <Release bin dir with savanna> <output dir>
#
# Makes two copies of the bin dir whose standard.frag is recompiled with one
# experiment define (VE_PERF_NO_TRANSLUCENCY / VE_PERF_NO_DISCARD), then runs
# Savanna's benchmark mode (VE_BENCH) over a few views with each variant,
# interleaved, PERF_REPEATS times, and writes <output dir>/summary.md.
#
# Environment: PERF_RESOLUTION (2560x1440), PERF_FRAMES (300), PERF_WARMUP (150),
# PERF_REPEATS (3), PERF_SCENES (subset of: camp-sun jungle vista camp-noleaves).
set -u
BIN=${1:?Release bin dir (e.g. build-release/bin)}; OUT=${2:?output dir}
mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd)
BIN=$(cd "$BIN" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
RES=${PERF_RESOLUTION:-2560x1440}; FRAMES=${PERF_FRAMES:-300}; WARMUP=${PERF_WARMUP:-150}; REPEATS=${PERF_REPEATS:-3}
SCENES=${PERF_SCENES:-"camp-sun jungle vista camp-noleaves"}
EXE=savanna; [ -f "$BIN/savanna.exe" ] && EXE=savanna.exe
[ -f "$BIN/$EXE" ] || { echo "no $EXE in $BIN"; exit 1; }
PY=python3; command -v python3 >/dev/null || PY=python

# --- Variants: the same build, standard.frag recompiled with one define --------------------
make_variant() { # name define
    local dir="$OUT/bin-$1"
    rm -rf "${dir:?}"; mkdir -p "$dir"
    cp "$BIN/$EXE" "$dir/"; [ -f "$BIN/ve-source-assets.txt" ] && cp "$BIN/ve-source-assets.txt" "$dir/"
    cp -r "$BIN/assets" "$dir/" 2>/dev/null; cp -r "$BIN/shaders" "$dir/"
    cp "$BIN"/*.dll "$dir/" 2>/dev/null
    # The build's own glslc command (one argument per line), output redirected, plus the define.
    local args=() line
    while IFS= read -r line || [ -n "$line" ]; do
        line=${line%$'\r'}; [ -n "$line" ] && args+=("$line")
    done < "$BIN/shaders/standard.frag.spv.cmd"
    local n=${#args[@]} cmd=("${args[0]}" "-D$2")
    for ((i = 1; i < n; i++)); do
        case "${args[i]}" in
            *standard.frag.spv) cmd+=("$dir/shaders/standard.frag.spv") ;;
            *standard.frag.spv.d) cmd+=("$dir/shaders/standard.frag.spv.d") ;;
            *) cmd+=("${args[i]}") ;;
        esac
    done
    "${cmd[@]}" || { echo "glslc failed for $1"; exit 1; }
    cmp -s "$BIN/shaders/standard.frag.spv" "$dir/shaders/standard.frag.spv" && { echo "variant $1: shader unchanged?"; exit 1; }
    echo "variant $1 ready ($2)"
}
make_variant notrans VE_PERF_NO_TRANSLUCENCY
make_variant nodiscard VE_PERF_NO_DISCARD

# --- A scene without the Foliage object (no cutout trees / bushes: only opaque StandardMaterials)
NOLEAVES="$OUT/savanna-noleaves.json"
SRC_SCENE=$(cat "$BIN/ve-source-assets.txt" 2>/dev/null | tr -d '\r')/scenes/savanna.json
[ -f "$SRC_SCENE" ] || SRC_SCENE="$BIN/assets/scenes/savanna.json"
$PY - "$SRC_SCENE" "$NOLEAVES" <<'PYEOF'
import json, sys
s = json.load(open(sys.argv[1]))
s["objects"] = [o for o in s["objects"] if o.get("type") != "Foliage"]
json.dump(s, open(sys.argv[2], "w"), indent=1)
PYEOF

scene_env() { # scene -> env assignments
    case $1 in
        camp-sun)      echo "SAVANNA_TIME=17 SAVANNA_POSE=20,3,-5,90,10" ;;           # into the low sun, acacias backlit
        jungle)        echo "SAVANNA_TIME=11 SAVANNA_POSE=-1050,40,420,70,-8" ;;       # dense broadleaf canopy
        vista)         echo "SAVANNA_TIME=10 SAVANNA_POSE=0,45,260,0,-12" ;;           # wide view, many trees at LOD / impostors
        camp-noleaves) echo "SAVANNA_TIME=17.6 SAVANNA_SCENE=$NOLEAVES" ;;             # huts, villagers, rocks: opaque only
    esac
}

X=(); [ -z "${DISPLAY:-}" ] && [ "$(uname)" = Linux ] && command -v xvfb-run >/dev/null && X=(xvfb-run -a -s "-screen 0 2560x1440x24")
run() { # variant dir, label, scene, extra env...
    local dir=$1 label=$2 scene=$3; shift 3
    local log="$OUT/logs/$label.$scene.$rep.log"
    ( cd "$dir" && env VE_HEADLESS=1 VE_RESOLUTION=$RES VE_BENCH=$FRAMES,$WARMUP VE_FIXED_DT=0.016667 SAVANNA_WEATHER=0 \
        VE_SAVE_DIR="$OUT/saves" $(scene_env "$scene") "$@" "${X[@]}" "./$EXE" > "$log" 2>&1 )
    printf "  %-22s %-14s %s\n" "$label" "$scene" "$(grep -o 'gpu [0-9.]* ms$' "$log" | tail -1)"
}

mkdir -p "$OUT/logs"
for ((rep = 1; rep <= REPEATS; rep++)); do
    echo "== repeat $rep / $REPEATS"
    for scene in $SCENES; do
        run "$BIN" base-on "$scene" SAVANNA_BACKLIT=1
        run "$BIN" base-off "$scene" SAVANNA_BACKLIT=0
        run "$OUT/bin-notrans" notrans "$scene" SAVANNA_BACKLIT=0
        # Without discard the leaf cards would be solid quads: a different picture, not comparable.
        [ "$scene" = camp-noleaves ] && run "$OUT/bin-nodiscard" nodiscard "$scene" SAVANNA_BACKLIT=0
    done
done

$PY "$HERE/summarize.py" "$OUT" | tee "$OUT/summary.md"
