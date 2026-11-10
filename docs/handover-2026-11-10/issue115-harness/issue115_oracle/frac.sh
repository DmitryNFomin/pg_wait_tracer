#!/bin/bash
# frac.sh <label> <clients> <pgbench args...> — in-statement FRACTION of backend
# time from two independent instruments over the same steady state:
#   (1) bpftrace: sum of pgstat_report_activity(RUNNING)->(not RUNNING) spans
#   (2) PostgreSQL's own pg_stat_activity sampler (the #115 oracle)
# No tracer involved in either.
set -uo pipefail
LABEL="$1"; CL="$2"; shift 2
OUT=/root/xp/frac-$LABEL; mkdir -p "$OUT"
PSQL="sudo -u postgres PGPORT=5418 psql -U postgres -d postgres"

cat > "$OUT/bt" <<'EOF'
interval:s:11 { if (@go == 0) { @go = 1; @t0 = nsecs; } }
uprobe:/usr/lib/postgresql/18/bin/postgres:pgstat_report_activity
{
  if (arg0 == 3) { @raopen[pid] = nsecs; }
  else if (@raopen[pid] != 0) {
    $d = nsecs - @raopen[pid];
    if ($d < 5000000000 && @go == 1) { @open_ns = stats($d); }
    delete(@raopen[pid]);
  }
}
END { printf("BTWIN %lld\n", nsecs - @t0); clear(@raopen); clear(@go); clear(@t0); }
EOF

bpftrace "$OUT/bt" > "$OUT/bt.txt" 2>"$OUT/bt.err" &
BT=$!
sleep 6
# shellcheck disable=SC2086
sudo -u postgres PGPORT=5418 pgbench -U postgres -d postgres -c "$CL" -j 2 -T 70 "$@" >"$OUT/pgbench.log" 2>&1 &
PGB=$!
sleep 5
BT0=$(python3 -c 'import time;print(time.time_ns())')
$PSQL -q -c "DROP TABLE IF EXISTS o2; CREATE UNLOGGED TABLE o2(ts timestamptz, pid int, state text, wet text, we text);" >/dev/null 2>&1
$PSQL -q -c "DO \$\$ DECLARE te timestamptz := clock_timestamp() + interval '40 seconds'; tick timestamptz; BEGIN LOOP EXIT WHEN clock_timestamp() > te; PERFORM pg_stat_clear_snapshot(); tick := clock_timestamp(); INSERT INTO o2 SELECT tick, pid, state, wait_event_type, wait_event FROM pg_stat_activity WHERE backend_type='client backend' AND pid <> pg_backend_pid(); PERFORM pg_sleep(0.02); END LOOP; END \$\$;" >"$OUT/o2.log" 2>&1
kill -INT $BT 2>/dev/null; wait $BT 2>/dev/null
BT1=$(python3 -c 'import time;print(time.time_ns())')
$PSQL -tAF'|' -c "SELECT count(distinct ts), count(*), count(*) FILTER (WHERE state='active' AND coalesce(we,'')<>'ClientRead' AND coalesce(wet,'')<>'Activity') FROM o2" > "$OUT/o2.sum" 2>&1
$PSQL -q -c "DROP TABLE IF EXISTS o2" >/dev/null 2>&1
kill "$PGB" 2>/dev/null; wait "$PGB" 2>/dev/null
echo "=== frac $LABEL clients=$CL args=$* ==="
cat "$OUT/bt.txt" | grep -E "open_ns"
echo "oracle(ticks|rows|active): $(cat "$OUT/o2.sum")"
python3 - "$OUT" "$BT0" "$BT1" "$CL" <<'PYEOF'
import sys, re
out, bt0, bt1, cl = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
txt = open(f"{out}/bt.txt").read()
m = re.search(r"@open_ns: count (\d+), average (\d+), total (\d+)", txt)
t, r, a = open(f"{out}/o2.sum").read().strip().split("|")
t, r, a = int(t), int(r), int(a)
win = (bt1 - bt0) / 1e9
if m:
    cnt, avg, tot = int(m.group(1)), int(m.group(2)), int(m.group(3))
    print(f"BPFTRACE  spans={cnt} mean={avg/1e3:.1f}us total={tot/1e9:.2f}s window={win:.2f}s "
          f"backends={cl} in_stmt_frac={100.0*tot/1e9/(win*cl):.2f}%")
    mw = re.search(r"BTWIN (\d+)", txt)
    if mw:
        w2 = int(mw.group(1)) / 1e9
        print(f"BPFTRACE  accum_window={w2:.2f}s  in_stmt_frac={100.0*tot/1e9/(w2*cl):.2f}%  "
              f"cmds_per_backend_per_s={cnt/w2/cl:.1f}")
print(f"ORACLE    ticks={t} rows={r} backends={r/t:.2f} active={a} in_stmt_frac={100.0*a/r:.2f}%")
PYEOF
