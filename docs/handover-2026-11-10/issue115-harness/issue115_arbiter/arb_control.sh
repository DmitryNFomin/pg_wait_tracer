#!/bin/bash
# arb_control.sh — the SAME workload and the SAME two non-tracer measurements
# (kernel schedstat, pg_stat_activity) with NO tracer attached at all.
# Difference against arb_capture.sh isolates the tracer's own perturbation,
# which lands in the traced BACKENDS (watchpoint traps), not just the daemon.
set -uo pipefail
cd /root/pgwt-arb || exit 1
source tests/testutil.sh

ESC=${ESC:-60}
CLIENTS=${CLIENTS:-16}
OUT=${OUT:-/root/arb/ctl1}

PM_PID=$(find_postmaster)
PORT=$(awk 'NR==4{print $1}' "$(readlink -f /proc/$PM_PID/cwd)/postmaster.pid" 2>/dev/null)
[[ -n "$PORT" ]] || PORT=5418
export PGPORT="$PORT"
echo "CONTROL postmaster=$PM_PID port=$PGPORT esc=$ESC clients=$CLIENTS out=$OUT"

rm -rf "$OUT"; mkdir -p "$OUT"
pgbench -U postgres -d postgres -c "$CLIENTS" -j 4 -T $((ESC + 9)) >"$OUT/pgbench.log" 2>&1 &
PGB=$!
sleep 3
python3 /root/arb/kernel_cpu_arbiter.py sample --postmaster "$PM_PID" \
    --out "$OUT/kernel.csv" --hz 20 --duration $((ESC + 8)) >"$OUT/kernel.log" 2>&1 &
KPID=$!
sleep 2

cat > "$OUT/oracle.sql" <<EOF
DROP TABLE IF EXISTS pgwt_oracle;
CREATE UNLOGGED TABLE pgwt_oracle(it int, ts timestamptz, pid int, state text, wet text, we text);
DROP TABLE IF EXISTS pgwt_oracle_meta;
CREATE UNLOGGED TABLE pgwt_oracle_meta(k text, v text);
INSERT INTO pgwt_oracle_meta VALUES ('sampler_pid', pg_backend_pid()::text);
DO \$\$
DECLARE t_end timestamptz := clock_timestamp() + interval '${ESC} seconds';
        it int := 0;
        tick timestamptz;
BEGIN
  LOOP
    EXIT WHEN clock_timestamp() > t_end;
    PERFORM pg_stat_clear_snapshot();
    it := it + 1;
    tick := clock_timestamp();
    INSERT INTO pgwt_oracle
      SELECT it, tick, pid, state, wait_event_type, wait_event
        FROM pg_stat_activity
       WHERE backend_type = 'client backend' AND pid <> pg_backend_pid();
    PERFORM pg_sleep(0.02);
  END LOOP;
END \$\$;
EOF
python3 -c 'import time;print(time.time_ns())' > "$OUT/w_from"
psql -U postgres -d postgres -q -f "$OUT/oracle.sql" >"$OUT/oracle.log" 2>&1
python3 -c 'import time;print(time.time_ns())' > "$OUT/w_to"

wait "$KPID" 2>/dev/null
kill "$PGB" 2>/dev/null; wait "$PGB" 2>/dev/null
psql -U postgres -d postgres -tAF, -c \
  "COPY (SELECT it, (extract(epoch from ts)*1e9)::bigint, pid, state, coalesce(wet,''), coalesce(we,'') FROM pgwt_oracle) TO STDOUT WITH (FORMAT csv)" \
  > "$OUT/oracle.csv" 2>>"$OUT/oracle.log"
psql -U postgres -d postgres -tAc "SELECT v FROM pgwt_oracle_meta WHERE k='sampler_pid'" > "$OUT/sampler_pid" 2>>"$OUT/oracle.log"
psql -U postgres -d postgres -q -c "DROP TABLE IF EXISTS pgwt_oracle; DROP TABLE IF EXISTS pgwt_oracle_meta" >>"$OUT/oracle.log" 2>&1
echo "oracle rows: $(wc -l < "$OUT/oracle.csv")  kernel rows: $(wc -l < "$OUT/kernel.csv")"
echo "tps: $(grep -i 'tps' "$OUT/pgbench.log" | head -2 | tr '\n' ' ')"
echo "CONTROL_OK $OUT"
