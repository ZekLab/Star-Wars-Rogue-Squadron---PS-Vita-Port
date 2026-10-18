#!/bin/sh
# Parser bench on the Mac: draw checksum (1 pass) + ms/pass (REPEAT passes) for every capture in build-mac/vg_cap_*.
# Usage: tools/vita/bench_replay.sh [repeat]   (run from the repo root; build vg_replay in build-vg-replay first)
REPEAT=${1:-400}
BIN=build-vg-replay/vg_replay
OUT=${TMPDIR:-/tmp}/vg_bench.png
for cap in build-mac/vg_cap_*; do
    name=$(basename "$cap")
    sum=$(VG_REPLAY_DRY=1 VG_REPLAY_CHECKSUM=1 "$BIN" "$cap" "$OUT" 2>&1 | grep -i checksum | tail -1)
    ms=$(VG_REPLAY_DRY=1 VG_REPLAY_REPEAT=$REPEAT "$BIN" "$cap" "$OUT" 2>&1 | grep "ms per pass" | sed -E 's/replay: ([0-9.]+) ms.*/\1/')
    printf '%-22s %8s ms  %s\n' "$name" "$ms" "$sum"
done
