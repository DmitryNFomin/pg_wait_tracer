#!/bin/bash
# analyze.sh <run_dir>  — gate variants + the PostgreSQL oracle over the same window
set -uo pipefail
R="$1"
P=$(cat "$R/sampler_pid")
OUT=$(/root/xp/xp_gate "$R/trace" --exclude-pid "$P" --client-only)
echo "$OUT"
WF=$(sed -n 's/.*wall_from=\([0-9]*\).*/\1/p' <<<"$OUT")
WT=$(sed -n 's/.*wall_to=\([0-9]*\).*/\1/p' <<<"$OUT")
echo ""
python3 /root/xp/oracle_share.py "$R/oracle.csv" "$WF" "$WT"
