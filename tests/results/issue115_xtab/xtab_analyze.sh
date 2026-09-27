#!/bin/bash
# xtab_analyze.sh <run_dir> <label> — issue #115.
# Window and pid set are chosen exactly as pgws_analyze.sh does, so the
# headline shares are comparable with the run that produced 67.99 / 62.27.
set -uo pipefail
R="$1"; L="${2:-run}"
SP=$(cat "$R/sampler_pid")
# Every figure is computed over the INSTRUMENTED backend set. With TRACKN
# unset that is all the pgbench backends; with TRACKN=k it is the k that the
# kernel dump covers, and the untracked ones are excluded on our side too
# (the EX loop below), so the two instruments never see different populations.
BP=$(cat "$R/bpids_track" 2>/dev/null || cat "$R/bpids")
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
echo "  pgsa sampler pid=$SP   excludes: ${#EX[@]} args"
echo "  tps: $(grep -i 'tps = ' "$R/pgbench.log" | head -1)"
echo ""
echo "--- exact + sampled tier (strict pgbench-backend set) ---"
/root/arb/xp_gate "$R/trace" --client-only "${EX[@]}" --win-from "$WF" --win-to "$WT" \
    --dump "$R/tier.dump" | grep -E '^(SAMPLED|EXACT|CMD-OPEN|INTERVALS|MARKERS|PIDS|IDLE|CPU\()'
echo ""
echo "--- kernel arbiter (schedstat counters, independent of the tracepoints) ---"
python3 /root/arb/kernel_cpu_arbiter.py report --kernel "$R/kernel.csv" --from "$WF" --to "$WT" --pids "$BP" | grep -E 'schedstat|f1\+f2|KERNEL'
echo ""
echo "--- reference: pg_stat_activity ---"
python3 /root/arb/pgsa_variants.py "$R/oracle.csv" "$WF" "$WT" --pids "$BP" --label "pgsa" | grep -E '^PGSA_(IN|V1|V2)'
echo ""
echo "--- headline from the aggregates only (schedstat; no tracepoint dump) ---"
# Exactly the two published quantities:
#   kernel_cpu_share_f1f2     = (f1 + f2) / (f1 + f2 + W)
#   exact_whole_backend_share = (exactC_cpu + cpu_out_cmd) / (that + W)
/root/arb/xp_gate "$R/trace" --client-only "${EX[@]}" --win-from "$WF" --win-to "$WT" > "$R/agg_gate.txt"
python3 /root/arb/kernel_cpu_arbiter.py report --kernel "$R/kernel.csv" --from "$WF" --to "$WT" --pids "$BP" > "$R/agg_kern.txt"
python3 - "$L" "$R/agg_gate.txt" "$R/agg_kern.txt" <<'PY'
import re, sys
label, gp, kp = sys.argv[1:4]
g = open(gp).read(); k = open(kp).read()
W = float(re.search(r'wait inside cmd = ([\d.]+) s', g).group(1))
out = float(re.search(r'cpu outside cmd = ([\d.]+) s', g).group(1))
cc = float(re.search(r'EXACT C .*cpu=([\d.]+)s', g).group(1))
f1 = float(re.search(r'sum_exec_runtime.*=\s+([\d.]+) s', k).group(1))
f2 = float(re.search(r'run_delay.*=\s+([\d.]+) s', k).group(1))
E, K_ = cc + out, f1 + f2
se, sk = 100*E/(E+W), 100*K_/(K_+W)
print(f"HEADLINE_AGG {label} exactWB={se:.2f}% kernel_f1f2={sk:.2f}% gap={sk-se:.2f} pp "
      f"(E={E:.3f}s K={K_:.3f}s W={W:.3f}s f1={f1:.3f}s f2={f2:.3f}s)")
PY
echo ""
if grep -q '^K ' "$R/sched.txt" 2>/dev/null; then
  echo "--- the cross-tab and the equation ---"
  python3 /root/arb/xtab_join.py --dump "$R/tier.dump" --bt "$R/sched.txt" \
      --pids "$BP" --from "$WF" --to "$WT" --mono-ns "$MONO" --wall-ns "$WALL" --label "$L"
else
  echo "--- no per-transition dump in this run (NODUMP=1 control) ---"
fi
echo ""
echo "--- instrument overhead ---"
for who in bt_pid daemon_pid; do
  P=$(cat "$R/$who" 2>/dev/null)
  [[ -n "$P" ]] && echo "  $who=$P $(python3 /root/arb/kernel_cpu_arbiter.py report --kernel "$R/kernel.csv" --from "$WF" --to "$WT" --pids "$P" | grep -E 'sum_exec_runtime|run_delay' | tr -s ' ' | tr '\n' ' ')"
done
echo ""
