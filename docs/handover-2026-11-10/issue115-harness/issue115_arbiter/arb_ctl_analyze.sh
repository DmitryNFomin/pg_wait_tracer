#!/bin/bash
# arb_ctl_analyze.sh <ctl_dir> — kernel vs pg_stat_activity, no tracer.
set -uo pipefail
R="$1"
SP=$(cat "$R/sampler_pid")
# trim 2 s off each end so both samplers certainly cover the window
WF=$(( $(cat "$R/w_from") + 2000000000 ))
WT=$(( $(cat "$R/w_to")   - 2000000000 ))
# pid set = every client backend pg_stat_activity saw inside the window,
# minus its own sampler backend: the same population the traced runs use.
PIDS=$(awk -F, -v a="$WF" -v b="$WT" -v s="$SP" \
        '$2>=a && $2<b && $3!=s {print $3}' "$R/oracle.csv" | sort -un | paste -sd, -)
echo "CONTROL window=[$WF,$WT) secs=$(python3 -c "print(($WT-$WF)/1e9)")"
echo "  pid set (pg_stat_activity client backends, sampler $SP excluded): $PIDS"
python3 /root/arb/kernel_cpu_arbiter.py report --kernel "$R/kernel.csv" \
    --from "$WF" --to "$WT" --pids "$PIDS" --oracle "$R/oracle.csv"
