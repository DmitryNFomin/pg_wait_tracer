#!/bin/bash
# analyze3.sh <run_dir> — all three sources restricted to the SAME wall window
set -uo pipefail
R="$1"
P=$(cat "$R/sampler_pid" 2>/dev/null || echo 0)
. "$R/window"
OUT=$(/root/xp/xp_gate "$R/trace" --exclude-pid "$P" --client-only --win-from "$T0" --win-to "$T1")
echo "$OUT"
echo ""
if [[ -s "$R/oracle.csv" ]]; then
  python3 /root/xp/oracle_share.py "$R/oracle.csv" "$T0" "$T1"
fi
echo ""
echo "PG-SIDE xact_commit=$XACT   pgss(sum calls|exec_ms|plan_ms)=$(cat "$R/pgss.sum")"
awk -F'|' -v t0="$T0" -v t1="$T1" 'BEGIN{}' </dev/null
python3 - "$R" "$T0" "$T1" <<'PYEOF'
import sys
r, t0, t1 = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
line = open(f"{r}/pgss.sum").read().strip().split("|")
calls, exec_ms, plan_ms = int(line[0]), float(line[1]), float(line[2])
win = (t1 - t0) / 1e9
print(f"PG-SIDE window={win:.3f}s calls={calls} ({calls/win:.0f}/s) "
      f"exec+plan={(exec_ms+plan_ms)/1e3:.3f} backend-s  mean_per_call={(exec_ms+plan_ms)*1e3/calls:.1f} us")
PYEOF
