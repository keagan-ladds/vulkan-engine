#!/bin/bash
# Two-process multiplayer smoke test on one machine: a Savanna host and a
# client that joins it over localhost (scripted by NetTest, Multiplayer.hpp).
#   tools/nettest.sh <bin dir with savanna> <output dir>
# Headless (VE_HEADLESS=1) and under xvfb-run if it is installed.
# Exit 0 when both processes finish their scripts with no "NO" / TIMEOUT.
# NETTEST_SIM="latency=80,jitter=20,loss=0.05": both run over a bad network (VE_NET_SIM).
# NETTEST_SERVER=1: the host runs in dedicated server mode (VE_SERVER).
set -u
BIN=${1:?bin dir}; OUT=${2:?output dir}
mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd) # (absolute: we cd into the bin dir)
PORT=${NETTEST_PORT:-17777}
X=(); command -v xvfb-run >/dev/null && X=(xvfb-run -a -s "-screen 0 1280x720x24")
cd "$BIN"
VE_NET_SIM=${NETTEST_SIM:-} VE_SERVER=${NETTEST_SERVER:-0} VE_HEADLESS=1 SAVANNA_NET=host:$PORT SAVANNA_NAME=Hosting SAVANNA_NETTEST=host SAVANNA_SHOTS="$OUT" VE_SAVE_DIR="$OUT/host-saves" \
    timeout 1200 "${X[@]}" ./savanna > "$OUT/host.log" 2>&1 &
HOST=$!
sleep "${NETTEST_DELAY:-8}" # the host is up (it queues a client that's early anyway)
VE_NET_SIM=${NETTEST_SIM:-} VE_HEADLESS=1 SAVANNA_NET=join:127.0.0.1:$PORT SAVANNA_NAME=Visitor SAVANNA_NETTEST=client SAVANNA_SHOTS="$OUT" VE_SAVE_DIR="$OUT/client-saves" \
    timeout 1200 "${X[@]}" ./savanna > "$OUT/client.log" 2>&1
CLIENT=$?
wait $HOST; HOSTEXIT=$?
grep -h "\[nettest" "$OUT/host.log" "$OUT/client.log"
echo "host exit=$HOSTEXIT client exit=$CLIENT vulkan=$(cat "$OUT"/*.log | grep -c '\[vulkan\]')"
if [ $HOSTEXIT -ne 0 ] || [ $CLIENT -ne 0 ] || grep -hE "\[nettest.*(NO\b|TIMEOUT|FAILED)" "$OUT/host.log" "$OUT/client.log" >/dev/null; then exit 1; fi
