#!/bin/bash
# capture2.sh — capture.sh + /proc schedstat run-queue accounting + pluggable workload
set -uo pipefail
cd /root/pgwt-check/cpu || exit 1
source tests/testutil.sh

RATE=${RATE:-50}
ESC=${ESC:-60}
CLIENTS=${CLIENTS:-8}
OUT=${OUT:-/root/xp/run1}
PGBARGS=${PGBARGS:-}          # extra pgbench args, e.g. "-f /root/xp/sleep.sql"

PM_PID=$(find_postmaster)
[[ -n "$PM_PID" ]] || { echo "no postmaster"; exit 1; }
PORT=$(awk 'NR==4{print $1}' "$(readlink -f /proc/$PM_PID/cwd)/postmaster.pid" 2>/dev/null)
[[ -n "$PORT" ]] || PORT=5418
export PGPORT="$PORT"
echo "postmaster=$PM_PID port=$PGPORT rate=$RATE esc=$ESC clients=$CLIENTS out=$OUT args=$PGBARGS"

rm -rf "$OUT"; mkdir -p "$OUT/trace"
TRACE="$OUT/trace"
SOCK="$TRACE/pgwt.sock"

./pg_wait_tracer --daemon --pid "$PM_PID" -i 1 -T "$TRACE" \
    --mode tiered --sample-rate "$RATE" --escalation-budget 900 -q \
    >/dev/null 2>"$OUT/daemon.log" &
TPID=$!
for _ in $(seq 1 40); do [[ -S "$SOCK" ]] && break; sleep 0.3; done
[[ -S "$SOCK" ]] || { echo "daemon did not start"; tail -20 "$OUT/daemon.log"; kill $TPID 2>/dev/null; exit 1; }

# shellcheck disable=SC2086
pgbench -U postgres -d postgres -c "$CLIENTS" -j 2 -T $((ESC + 12)) $PGBARGS >"$OUT/pgbench.log" 2>&1 &
PGB=$!
sleep 3

# schedstat snapshot: all client backends, BEFORE the window
snap_sched() {
    local f="$1"
    : > "$f"
    for p in $(pgrep -P "$PM_PID" 2>/dev/null); do
        # only client backends
        if grep -q "postgres: postgres postgres" /proc/$p/cmdline 2>/dev/null || \
           tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null | grep -q "postgres postgres"; then
            echo "$p $(cat /proc/$p/schedstat 2>/dev/null)" >> "$f"
        fi
    done
}
snap_sched "$OUT/sched.before"

python3 - "$SOCK" "{\"cmd\":\"escalate\",\"duration_s\":${ESC},\"reason\":\"cpu-oracle\"}" <<'PYEOF'
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.settimeout(5)
s.connect(sys.argv[1]); s.sendall((sys.argv[2] + "\n").encode())
buf = b""
while b"\n" not in buf:
    c = s.recv(4096)
    if not c: break
    buf += c
print("escalate:", buf.decode().split("\n")[0])
PYEOF

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
psql -U postgres -d postgres -q -f "$OUT/oracle.sql" >"$OUT/oracle.log" 2>&1

snap_sched "$OUT/sched.after"

sleep 3
kill "$PGB" 2>/dev/null; wait "$PGB" 2>/dev/null
kill -TERM "$TPID" 2>/dev/null; wait "$TPID" 2>/dev/null

psql -U postgres -d postgres -tAF, -c \
  "COPY (SELECT it, (extract(epoch from ts)*1e9)::bigint, pid, state, coalesce(wet,''), coalesce(we,'') FROM pgwt_oracle) TO STDOUT WITH (FORMAT csv)" \
  > "$OUT/oracle.csv" 2>>"$OUT/oracle.log"
psql -U postgres -d postgres -tAc "SELECT v FROM pgwt_oracle_meta WHERE k='sampler_pid'" > "$OUT/sampler_pid" 2>>"$OUT/oracle.log"
psql -U postgres -d postgres -q -c "DROP TABLE IF EXISTS pgwt_oracle; DROP TABLE IF EXISTS pgwt_oracle_meta" >>"$OUT/oracle.log" 2>&1
echo "oracle rows: $(wc -l < "$OUT/oracle.csv")  sampler_pid=$(cat "$OUT/sampler_pid")"

# run-queue delta over the window
python3 - "$OUT/sched.before" "$OUT/sched.after" <<'PYEOF'
import sys
def load(p):
    d = {}
    for line in open(p):
        f = line.split()
        if len(f) >= 4:
            d[f[0]] = (int(f[1]), int(f[2]), int(f[3]))
    return d
a, b = load(sys.argv[1]), load(sys.argv[2])
ron = rq = slices = 0
n = 0
for pid, (r1, q1, s1) in a.items():
    if pid not in b: continue
    r2, q2, s2 = b[pid]
    ron += r2 - r1; rq += q2 - q1; slices += s2 - s1
    n += 1
print(f"SCHEDSTAT pids={n} cpu_run={ron/1e9:.3f}s runqueue_wait={rq/1e9:.3f}s slices={slices} "
      f"rq_pct_of_run={100.0*rq/ron if ron else 0:.1f}%")
PYEOF
echo "CAPTURE_OK $OUT"
