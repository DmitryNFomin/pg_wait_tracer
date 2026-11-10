#!/bin/bash
# bias_analyze.sh <run_dir> — five observers over one window, one pid set.
set -uo pipefail
R="$1"
SP=$(cat "$R/sampler_pid"); DP=$(cat "$R/daemon_pid"); BP=$(cat "$R/bpids")
NB=$(tr ',' '\n' <<<"$BP" | wc -l | tr -d ' ')
read -r MONO WALL < "$R/clockoff"

# window + the candidate pid set from the trace itself
P1=$(/root/arb/xp_gate "$R/trace" --client-only --per-pid)
WF=$(sed -n 's/.*wall_from=\([0-9]*\).*/\1/p' <<<"$P1")
WT=$(sed -n 's/.*wall_to=\([0-9]*\).*/\1/p' <<<"$P1")
# exclude everything the trace saw that is NOT one of the pgbench backends,
# so exact tier / sampled tier / pg_stat_activity / schedstat / tracepoints
# all sum over the identical 8 (or 2) backends.
EX=()
for p in $(grep '^PID ' <<<"$P1" | awk '{print $2}'); do
    grep -qx "$p" <(tr ',' '\n' <<<"$BP") || EX+=(--exclude-pid "$p")
done
echo "ALIGNMENT window=[$WF,$WT) secs=$(python3 -c "print(($WT-$WF)/1e9)") backends=$NB: $BP"
echo "  excluded from the trace side: ${EX[*]:-none}   (pgsa sampler=$SP, daemon=$DP)"
echo ""
/root/arb/xp_gate "$R/trace" --client-only "${EX[@]}" | grep -vE '^PID '
echo ""
python3 /root/arb/kernel_cpu_arbiter.py sched --bt "$R/sched.txt" \
    --from "$WF" --to "$WT" --pids "$BP" --mono-ns "$MONO" --wall-ns "$WALL"
echo ""
python3 /root/arb/kernel_cpu_arbiter.py report --kernel "$R/kernel.csv" \
    --from "$WF" --to "$WT" --pids "$BP" --oracle "$R/oracle.csv"
echo ""
python3 /root/arb/kernel_cpu_arbiter.py sampbias --bt "$R/sched.txt" \
    --from "$WF" --to "$WT" --pids "$BP" --mono-ns "$MONO" --wall-ns "$WALL"
echo ""
python3 /root/arb/kernel_cpu_arbiter.py torn --oracle "$R/oracle.csv" --from "$WF" --to "$WT"
echo ""
echo "--- sampler wakeup->on-cpu delay (whole bpftrace run) ---"
sed -n '/SDELAY_US/,$p' "$R/sched.txt" | head -20
echo "--- observer costs over the window (on-CPU s) ---"
for x in "daemon $DP" "bpftrace $(cat "$R/bt_pid")" "pgsa-sampler $SP"; do
  set -- $x
  echo -n "$1: "
  python3 /root/arb/kernel_cpu_arbiter.py report --kernel "$R/kernel.csv" \
      --from "$WF" --to "$WT" --pids "$2" 2>/dev/null | sed -n '2p' | tr -s ' '
done
