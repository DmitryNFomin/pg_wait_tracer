#!/bin/bash
# xtab_run.sh [REPEATS] — issue #115: N interleaved saturated/unsaturated runs.
# MODES defaults to "dump nodump": the dump run carries the decomposition, the
# paired nodump run measures how much the dump's own printf perturbs the
# workload (its headline is computed from the aggregate maps alone).
set -uo pipefail
N=${1:-3}
ESC=${ESC:-45}
SAT_CLIENTS=${SAT_CLIENTS:-8}
UNSAT_CLIENTS=${UNSAT_CLIENTS:-2}
MODES=${MODES:-"dump nodump"}
mkdir -p /root/arb/xtab
for i in $(seq 1 "$N"); do
  for cfg in sat unsat; do
    if [[ $cfg == sat ]]; then CL=$SAT_CLIENTS; else CL=$UNSAT_CLIENTS; fi
    for mode in $MODES; do
      ND=0; [[ $mode == nodump ]] && ND=1
      TN=$CL
      [[ $mode == half ]] && { TN=$(( (CL + 1) / 2 )); ND=0; }
      [[ $mode == quarter ]] && { TN=$(( (CL + 3) / 4 )); ND=0; }
      D=/root/arb/xtab/$cfg$mode$i
      echo "=== capture $cfg$mode$i clients=$CL esc=$ESC nodump=$ND trackn=$TN ==="
      TRACKN=$TN NODUMP=$ND ESC=$ESC CLIENTS=$CL OUT=$D bash /root/arb/xtab_capture.sh 2>&1 | tail -4
      ESC_S=$ESC bash /root/arb/xtab_analyze.sh "$D" "$cfg$mode$i" > "/root/arb/xtab/$cfg$mode$i.txt" 2>&1
      grep -E 'HEADLINE|MISMATCH|FATAL|Traceback' "/root/arb/xtab/$cfg$mode$i.txt" | head -5
      rm -f "$D/tier.dump" "$D/sched.txt"
    done
  done
done
echo "ALL DONE"
