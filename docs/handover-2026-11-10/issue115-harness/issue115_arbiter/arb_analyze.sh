#!/bin/bash
# arb_analyze.sh <run_dir> — all four sources over one identical window and one
# identical pid set.
#
# Alignment procedure (two passes, because the kernel arbiter is the only
# source that can *fail* to cover a pid):
#   1. xp_gate defines the window [wall_from, wall_to) = the intersection of
#      the transitions-block and samples-block coverage, and the candidate pid
#      set (--client-only = pids that emitted command markers).
#   2. the kernel reporter says which of those pids its counter samples do not
#      span end-to-end (a backend that started or exited mid-window).
#   3. xp_gate is re-run with those pids ALSO excluded, so the exact tier, the
#      sampled tier, pg_stat_activity and the kernel all sum over exactly the
#      same backends over exactly the same nanoseconds.
set -uo pipefail
R="$1"
SP=$(cat "$R/sampler_pid")
DP=$(cat "$R/daemon_pid")

P1=$(/root/arb/xp_gate "$R/trace" --exclude-pid "$SP" --client-only --per-pid)
WF=$(sed -n 's/.*wall_from=\([0-9]*\).*/\1/p' <<<"$P1")
WT=$(sed -n 's/.*wall_to=\([0-9]*\).*/\1/p' <<<"$P1")
PIDS=$(grep '^PID ' <<<"$P1" | awk '{print $2}' | paste -sd, -)

DROP=$(python3 /root/arb/kernel_cpu_arbiter.py report --kernel "$R/kernel.csv" \
        --from "$WF" --to "$WT" --pids "$PIDS" --exclude-pid "$SP" \
        | sed -n 's/.*excluded: \[\(.*\)\]).*/\1/p' | tr -d ' ')
EX=(--exclude-pid "$SP")
KEEP="$PIDS"
if [[ -n "$DROP" ]]; then
    for p in ${DROP//,/ }; do EX+=(--exclude-pid "$p"); done
    KEEP=$(python3 -c "
import sys
a=set(sys.argv[1].split(','));b=set(sys.argv[2].split(','))
print(','.join(sorted(a-b,key=int)))" "$PIDS" "$DROP")
fi

echo "ALIGNMENT window=[$WF,$WT)  secs=$(python3 -c "print(($WT-$WF)/1e9)")"
echo "  candidate client pids (xp_gate --client-only): $PIDS"
echo "  dropped, not spanning the window in kernel samples: ${DROP:-none}"
echo "  FINAL common pid set: $KEEP"
echo "  always excluded: pg_stat_activity sampler backend=$SP, tracer daemon=$DP"
echo ""
/root/arb/xp_gate "$R/trace" "${EX[@]}" --client-only | grep -v '^PID '
echo ""
python3 /root/arb/kernel_cpu_arbiter.py report --kernel "$R/kernel.csv" \
    --from "$WF" --to "$WT" --pids "$KEEP" --oracle "$R/oracle.csv"
if [[ -f "$R/sched.txt" ]]; then
    echo ""
    read -r MONO WALL < "$R/clockoff"
    python3 /root/arb/kernel_cpu_arbiter.py sched --bt "$R/sched.txt" \
        --from "$WF" --to "$WT" --pids "$KEEP" --mono-ns "$MONO" --wall-ns "$WALL"
fi
echo ""
echo "--- tracer daemon's own consumption (excluded from every figure above) ---"
python3 /root/arb/kernel_cpu_arbiter.py report --kernel "$R/kernel.csv" \
    --from "$WF" --to "$WT" --pids "$DP" 2>&1 | sed -n '2,7p'
