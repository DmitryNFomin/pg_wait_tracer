#!/bin/bash
# run_offset.sh <label> <secs> <pgbench args...>
set -uo pipefail
LABEL="$1"; SECS="$2"; shift 2
export PGPORT=5418
OUT=/root/xp/off-$LABEL
mkdir -p "$OUT"
sudo -u postgres PGPORT=5418 psql -U postgres -d postgres -q -c "select pg_stat_statements_reset()" >/dev/null 2>&1
B=$(sudo -u postgres PGPORT=5418 psql -U postgres -d postgres -tAc "select xact_commit from pg_stat_database where datname='postgres'")
bpftrace /root/xp/offset.bt > "$OUT/bt.txt" 2>"$OUT/bt.err" &
BT=$!
sleep 6
sudo -u postgres PGPORT=5418 pgbench -U postgres -d postgres -T "$SECS" "$@" > "$OUT/pgbench.log" 2>&1
A=$(sudo -u postgres PGPORT=5418 psql -U postgres -d postgres -tAc "select xact_commit from pg_stat_database where datname='postgres'")
kill -INT $BT 2>/dev/null; wait $BT 2>/dev/null
sudo -u postgres PGPORT=5418 psql -U postgres -d postgres -tAF'|' -c \
  "select calls, round(total_exec_time::numeric,1), round(total_plan_time::numeric,1), left(query,40) from pg_stat_statements where calls>10 order by calls desc limit 8" \
  > "$OUT/pgss.txt" 2>&1
echo "=== $LABEL  xact_commit_delta=$((A-B)) ==="
grep -E "^(transaction type|scaling|number of transactions actually|latency average|tps)" "$OUT/pgbench.log"
echo "--- pg_stat_statements (calls|exec_ms|plan_ms|query) ---"
cat "$OUT/pgss.txt"
echo "--- bpftrace ---"
cat "$OUT/bt.txt"
