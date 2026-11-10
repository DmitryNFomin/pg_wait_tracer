#!/bin/bash
# btwin.sh <run_dir> — ground-truth in-statement time from PostgreSQL's own
# pgstat_report_activity calls, restricted to the run's [T0,T1] wall window.
# Reads bt.buckets (100 ms monotonic buckets) + clock.offset written by capture4.
set -uo pipefail
R="$1"
. "$R/window"
python3 - "$R" "$T0" "$T1" <<'PYEOF'
import sys, re
r, t0, t1 = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
off = int(open(f"{r}/clock.offset").read().strip())   # realtime_ns - monotonic_ns
m0, m1 = t0 - off, t1 - off
tot = 0; cnt = 0
txt = open(f"{r}/bt.txt").read()
cur = None
for line in txt.splitlines():
    mm = re.match(r"@(sec|secn)\[(\d+)\]: (\d+)", line.strip())
    if not mm: continue
    kind, b, v = mm.group(1), int(mm.group(2)), int(mm.group(3))
    tmid = b * 100_000_000 + 50_000_000
    if not (m0 <= tmid < m1): continue
    if kind == "sec": tot += v
    else: cnt += v
win = (t1 - t0) / 1e9
print(f"BPFTRACE-GT in_stmt_total={tot/1e9:.3f}s spans={cnt} mean={tot/cnt/1e3 if cnt else 0:.1f}us window={win:.3f}s")
PYEOF
