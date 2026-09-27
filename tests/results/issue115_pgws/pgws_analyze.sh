#!/bin/bash
# pgws_analyze.sh <run_dir> <label> — issue #115 follow-up.
#
# Same window, same pid set, scored by six observers; adds pg_wait_sampling
# and a second scheduling-bias measurement for its collector background
# worker alongside the pg_stat_activity poller's.
set -uo pipefail
R="$1"; L="${2:-run}"
SP=$(cat "$R/sampler_pid"); DP=$(cat "$R/daemon_pid"); BP=$(cat "$R/bpids")
CP=$(cat "$R/collector_pid"); NP=$(cat "$R/snap_pid")
NB=$(tr ',' '\n' <<<"$BP" | wc -l | tr -d ' ')
read -r MONO WALL < "$R/clockoff"

P1=$(/root/arb/xp_gate "$R/trace" --client-only --per-pid)
TF=$(sed -n 's/.*wall_from=\([0-9]*\).*/\1/p' <<<"$P1")
TT=$(sed -n 's/.*wall_to=\([0-9]*\).*/\1/p' <<<"$P1")
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
echo "  pgsa sampler pid=$SP  pgws collector pid=$CP  pgws snapshotter pid=$NP"
echo "  pgws settings: $(tr '\n' ' ' < "$R/pgws_settings")"
echo "  tps: $(grep -i 'tps = ' "$R/pgbench.log" | head -1)"
echo ""
echo "--- exact + sampled tier (strict pgbench-backend set) ---"
/root/arb/xp_gate "$R/trace" --client-only "${EX[@]}" --win-from "$WF" --win-to "$WT" | grep -E '^(SAMPLED|EXACT|CMD-OPEN|INTERVALS|MARKERS|PIDS)'
echo ""
echo "--- reference: pg_stat_activity (V1 in-command, V2 whole-backend) ---"
python3 /root/arb/pgsa_variants.py "$R/oracle.csv" "$WF" "$WT" --pids "$BP" --label "pgsa"
echo ""
echo "--- reference: pg_wait_sampling profile ---"
python3 /root/arb/pgws_share.py "$R/pgws.csv" "$WF" "$WT" --pids "$BP" --label "pgws"
echo ""
echo "--- kernel arbiter ---"
python3 /root/arb/kernel_cpu_arbiter.py report --kernel "$R/kernel.csv" --from "$WF" --to "$WT" --pids "$BP" | grep -E 'schedstat|f1\+f2|KERNEL'
python3 /root/arb/kernel_cpu_arbiter.py sched --bt "$R/sched.txt" --from "$WF" --to "$WT" --pids "$BP" --mono-ns "$MONO" --wall-ns "$WALL" | grep -E 'SCHEDTRACE|on-CPU|run-queue|off-CPU'
echo ""
echo "--- scheduling bias, the SAME instrument on both in-database samplers ---"
python3 /root/arb/kernel_cpu_arbiter.py sampbias --bt "$R/sched.txt" --from "$WF" --to "$WT" --pids "$BP" --mono-ns "$MONO" --wall-ns "$WALL" \
    --n-map sn --obs-map sobs --on-map son --tag pgsa | grep -E 'by time|switch-ins|SCHEDULING BIAS'
python3 /root/arb/kernel_cpu_arbiter.py sampbias --bt "$R/sched.txt" --from "$WF" --to "$WT" --pids "$BP" --mono-ns "$MONO" --wall-ns "$WALL" \
    --n-map cn --obs-map cobs --on-map con --tag pgws_all | grep -E 'by time|switch-ins|SCHEDULING BIAS'
python3 /root/arb/kernel_cpu_arbiter.py sampbias --bt "$R/sched.txt" --from "$WF" --to "$WT" --pids "$BP" --mono-ns "$MONO" --wall-ns "$WALL" \
    --n-map cnw --obs-map cobsw --on-map con --tag pgws_probe | grep -E 'by time|switch-ins|SCHEDULING BIAS'
echo ""
echo "--- collector scheduling detail ---"
python3 /root/arb/kernel_cpu_arbiter.py delays --bt "$R/sched.txt"
python3 - "$R/sched.txt" "$MONO" "$WALL" "$WF" "$WT" <<'PY'
import sys, importlib.util
spec = importlib.util.spec_from_file_location("k", "/root/arb/kernel_cpu_arbiter.py")
k = importlib.util.module_from_spec(spec); spec.loader.exec_module(k)
bt, mono, wall, wf, wt = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
m = k.parse_bt(bt, mono, wall)
win = (wt - wf) / 1e9
def d(name):
    v = k.delta(m[name].get(0, []), wf, wt)
    return 0.0 if v is None else v
print(f"COLLECTOR wakeups={d('cwake'):.0f} ({d('cwake')/win:.1f}/s)  "
      f"switch-ins={d('cn'):.0f} ({d('cn')/win:.1f}/s)  "
      f"wakeup-triggered switch-ins={d('cnw'):.0f} ({d('cnw')/win:.1f}/s)")
print(f"COLLECTOR on-CPU={d('con')/1e9:.3f}s = {100.0*d('con')/1e9/win:.2f}% of one processor")
print(f"PGSA_SAMPLER on-CPU={d('son')/1e9:.3f}s = {100.0*d('son')/1e9/win:.2f}% of one processor  "
      f"switch-ins={d('sn'):.0f} ({d('sn')/win:.1f}/s)")
PY
echo ""
echo "--- pg_wait_sampling collector overhead, kernel side ---"
python3 /root/arb/kernel_cpu_arbiter.py report --kernel "$R/kernel.csv" --from "$WF" --to "$WT" --pids "$CP" | grep -E 'schedstat|KERNEL'
echo ""
python3 /root/arb/kernel_cpu_arbiter.py torn --oracle "$R/oracle.csv" --from "$WF" --to "$WT"
echo ""
