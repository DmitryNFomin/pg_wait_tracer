#!/bin/bash
# recon_analyze.sh <run_dir> <label> — issue #115 reconciliation.
#
# ONE captured window, scored by BOTH prior investigations' methods at once:
#
#   M2 "arbiter"  exact side: xp_gate restricted to exactly the pgbench
#                 backends (every other traced pid excluded)
#                 reference side: pg_stat_activity over the same pid set
#   M1 "oracle"   exact side: xp_gate --client-only with only the
#                 pg_stat_activity sampler backend excluded
#                 reference side: oracle_share.py over EVERY client backend
#                 pg_stat_activity saw (its own sampler already filtered out
#                 by the capture SQL)
#
# plus the kernel arbiter (schedstat counter deltas, sched tracepoints),
# the directly measured sampler scheduling bias, and the pg_stat_activity
# classifier variants (V1..V4).
set -uo pipefail
R="$1"; L="${2:-run}"
SP=$(cat "$R/sampler_pid"); DP=$(cat "$R/daemon_pid"); BP=$(cat "$R/bpids")
NB=$(tr ',' '\n' <<<"$BP" | wc -l | tr -d ' ')
read -r MONO WALL < "$R/clockoff"

P1=$(/root/arb/xp_gate "$R/trace" --client-only --per-pid)
TF=$(sed -n 's/.*wall_from=\([0-9]*\).*/\1/p' <<<"$P1")
TT=$(sed -n 's/.*wall_to=\([0-9]*\).*/\1/p' <<<"$P1")
# The analysis window is the ESCALATION window (the only stretch the exact
# tier covers), trimmed 3 s at each end and then clamped to the interval every
# other observer actually spans -- the kernel sampler's per-backend coverage
# and the trace itself. All five observers therefore cover exactly the same
# wall interval; no observer is credited a second it did not watch.
ESC_S=${ESC_S:-45}
EF=$(cat "$R/esc_from")
read -r WF WT < <(python3 - "$R/kernel.csv" "$EF" "$ESC_S" "$TF" "$TT" "$BP" <<'PY'
import sys, csv, collections
kc, ef, esc, tf, tt, bp = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]), sys.argv[6]
want = set(int(x) for x in bp.split(',') if x.strip())
first, last = collections.defaultdict(lambda: None), {}
with open(kc) as f:
    rd = csv.reader(f); next(rd, None)
    for row in rd:
        p = int(row[1])
        if p not in want: continue
        t = int(row[0])
        if first[p] is None: first[p] = t
        last[p] = t
kf = max(v for v in first.values()) if first else tf
kt = min(last.values()) if last else tt
wf = max(ef + 3_000_000_000, tf, kf)
wt = min(ef + esc * 1_000_000_000 - 3_000_000_000, tt, kt)
print(wf, wt)
PY
)

EX=()
for p in $(grep '^PID ' <<<"$P1" | awk '{print $2}'); do
    grep -qx "$p" <(tr ',' '\n' <<<"$BP") || EX+=(--exclude-pid "$p")
done

echo "######## $L ########"
echo "WINDOW [$WF,$WT) secs=$(python3 -c "print(($WT-$WF)/1e9)") pgbench_backends=$NB: $BP"
echo "  M2 excludes from the trace side: ${EX[*]:-none}"
echo "  M1 excludes from the trace side: --exclude-pid $SP (the pgsa sampler) only"
echo "  tps: $(grep -i 'tps = ' "$R/pgbench.log" | head -1)"
echo ""
echo "--- M2 exact side (strict pgbench-backend set) ---"
/root/arb/xp_gate "$R/trace" --client-only "${EX[@]}" --win-from "$WF" --win-to "$WT" | grep -E '^(SAMPLED|EXACT|CMD-OPEN|INTERVALS|MARKERS|PIDS)'
echo "--- M2cap16 exact side (same list TRUNCATED to 16: emulates the pre-fix --exclude-pid cap) ---"
/root/arb/xp_gate "$R/trace" --client-only "${EX[@]:0:32}" --win-from "$WF" --win-to "$WT" | grep -E '^(SAMPLED|EXACT|CMD-OPEN)' | sed 's/^/CAP16 /'
echo "--- M1 exact side (--client-only, sampler excluded only) ---"
/root/arb/xp_gate "$R/trace" --client-only --exclude-pid "$SP" --win-from "$WF" --win-to "$WT" | grep -E '^(SAMPLED|EXACT|CMD-OPEN|INTERVALS|MARKERS|PIDS)'
echo ""
echo "--- reference side: pg_stat_activity, M2 pid set (strict) ---"
python3 /root/arb/pgsa_variants.py "$R/oracle.csv" "$WF" "$WT" --pids "$BP" --label "M2strict"
echo "--- reference side: pg_stat_activity, M1 pid set (every client backend) ---"
python3 /root/arb/pgsa_variants.py "$R/oracle.csv" "$WF" "$WT" --label "M1all"
echo "--- reference side: oracle_share.py verbatim (M1's own script, own spacing) ---"
python3 /root/arb/oracle_share.py "$R/oracle.csv" "$WF" "$WT" | head -3
echo ""
echo "--- kernel arbiter ---"
python3 /root/arb/kernel_cpu_arbiter.py report --kernel "$R/kernel.csv" --from "$WF" --to "$WT" --pids "$BP" | grep -E 'schedstat|f1\+f2|KERNEL'
python3 /root/arb/kernel_cpu_arbiter.py sched --bt "$R/sched.txt" --from "$WF" --to "$WT" --pids "$BP" --mono-ns "$MONO" --wall-ns "$WALL" | grep -E 'SCHEDTRACE|on-CPU|run-queue|off-CPU'
python3 /root/arb/kernel_cpu_arbiter.py sampbias --bt "$R/sched.txt" --from "$WF" --to "$WT" --pids "$BP" --mono-ns "$MONO" --wall-ns "$WALL" | grep -E 'by time|switch-ins|SCHEDULING BIAS'
python3 /root/arb/kernel_cpu_arbiter.py torn --oracle "$R/oracle.csv" --from "$WF" --to "$WT"
echo ""
