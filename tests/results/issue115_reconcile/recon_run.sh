#!/bin/bash
# recon_run.sh [REPEATS] — issue #115: one machine, one workload, both tiers,
# both prior methods, saturated and unsaturated, N interleaved repeats.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
N=${1:-3}
ESC=${ESC:-45}
SAT_CLIENTS=${SAT_CLIENTS:-8}
UNSAT_CLIENTS=${UNSAT_CLIENTS:-2}
OUTBASE=${OUTBASE:-/root/arb/recon}
mkdir -p "$OUTBASE"
for i in $(seq 1 "$N"); do
  for cfg in sat unsat; do
    if [[ $cfg == sat ]]; then CL=$SAT_CLIENTS; else CL=$UNSAT_CLIENTS; fi
    D="$OUTBASE/$cfg$i"
    echo "=== capture $cfg$i clients=$CL esc=$ESC ==="
    ESC=$ESC CLIENTS=$CL OUT=$D bash "$HERE/recon_capture.sh" 2>&1 | tail -4
    ESC_S=$ESC bash "$HERE/recon_analyze.sh" "$D" "$cfg$i" > "$OUTBASE/$cfg$i.txt" 2>&1
    tail -2 "$OUTBASE/$cfg$i.txt"
  done
done
echo "ALL DONE"
