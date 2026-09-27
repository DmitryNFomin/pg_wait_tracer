#!/bin/bash
# xtab_capture.sh — issue #115: capture one window with the exact tier and a
# per-transition kernel scheduler timeline for the SAME backends, so the two
# can be intersected (xtab_join.py) instead of compared in aggregate.
#
# Derived from tests/results/issue115_pgws/pgws_capture.sh. Differences:
#   - pg_wait_sampling is not involved (not the question here)
#   - sched3.bt.tmpl -> xtab.bt.tmpl, which DUMPS every tracked backend's
#     scheduler state change as well as keeping the aggregate maps
#   - the pg_stat_activity poller is kept, unchanged, so the workload matches
#     the runs that produced the 67.99 / 62.27 figures as closely as possible
set -uo pipefail
cd /root/pgwt-arb || exit 1
source tests/testutil.sh

RATE=${RATE:-50}
ESC=${ESC:-45}
CLIENTS=${CLIENTS:-8}
OUT=${OUT:-/root/arb/xtab1}

PM_PID=$(find_postmaster)
PORT=$(awk 'NR==4{print $1}' "$(readlink -f /proc/$PM_PID/cwd)/postmaster.pid" 2>/dev/null)
[[ -n "$PORT" ]] || PORT=5418
export PGPORT="$PORT"
rm -rf "$OUT"; mkdir -p "$OUT/trace"
TRACE="$OUT/trace"; SOCK="$TRACE/pgwt.sock"

pkill -f 'pgbench -U postgres' >/dev/null 2>&1
pkill -f 'psql .*sampler\.sql' >/dev/null 2>&1
pkill -x bpftrace >/dev/null 2>&1
pkill -f 'pg_wait_tracer --daemon' >/dev/null 2>&1
sleep 2
psql -U postgres -d postgres -tAc \
  "SELECT pg_terminate_backend(pid) FROM pg_stat_activity
    WHERE backend_type='client backend' AND pid <> pg_backend_pid()
      AND coalesce(application_name,'') IN ('pgbench','psql')" >/dev/null 2>&1
sleep 1

echo "postmaster=$PM_PID port=$PGPORT esc=$ESC clients=$CLIENTS out=$OUT nproc=$(nproc)"

pgbench -U postgres -d postgres -c "$CLIENTS" -j 4 -T $((ESC + 40)) >"$OUT/pgbench.log" 2>&1 &
PGB=$!
sleep 6

psql -U postgres -d postgres -q <<'EOF' >"$OUT/setup.log" 2>&1
DROP TABLE IF EXISTS pgwt_oracle; DROP TABLE IF EXISTS pgwt_oracle_meta;
CREATE UNLOGGED TABLE pgwt_oracle(it int, ts timestamptz, pid int, state text, wet text, we text);
CREATE UNLOGGED TABLE pgwt_oracle_meta(k text, v text);
EOF

cat > "$OUT/sampler.sql" <<EOF
INSERT INTO pgwt_oracle_meta VALUES ('sampler_pid', pg_backend_pid()::text);
DO \$\$
DECLARE t_end timestamptz := clock_timestamp() + interval '$((ESC + 25)) seconds';
        it int := 0; tick timestamptz;
BEGIN
  LOOP
    EXIT WHEN clock_timestamp() > t_end;
    PERFORM pg_stat_clear_snapshot();
    it := it + 1; tick := clock_timestamp();
    INSERT INTO pgwt_oracle
      SELECT it, tick, pid, state, wait_event_type, wait_event
        FROM pg_stat_activity
       WHERE backend_type = 'client backend' AND pid <> pg_backend_pid();
    PERFORM pg_sleep(0.02);
  END LOOP;
END \$\$;
EOF
psql -U postgres -d postgres -q -f "$OUT/sampler.sql" >"$OUT/oracle.log" 2>&1 &
SQLPID=$!

for _ in $(seq 1 50); do
    SAMPLER=$(psql -U postgres -d postgres -tAc "SELECT v FROM pgwt_oracle_meta WHERE k='sampler_pid'" 2>/dev/null | tr -d '[:space:]')
    [[ -n "$SAMPLER" ]] && break
    sleep 0.3
done
[[ -n "$SAMPLER" ]] || { echo "FATAL: no sampler pid"; exit 1; }
BPIDS=$(psql -U postgres -d postgres -tAc \
  "SELECT string_agg(pid::text, ',' ORDER BY pid) FROM pg_stat_activity
    WHERE backend_type='client backend' AND application_name='pgbench'" | tr -d '[:space:]')
echo "sampler=$SAMPLER pgbench_backends=$BPIDS"
echo "$SAMPLER" > "$OUT/sampler_pid"; echo "$BPIDS" > "$OUT/bpids"
[[ $(tr ',' '\n' <<<"$BPIDS" | wc -l) -eq $CLIENTS ]] || { echo "FATAL: backend count mismatch"; exit 1; }

# TRACKN=k instruments only the first k of the CLIENTS backends. The workload
# is unchanged (all CLIENTS backends still run); only the dump's own cost
# falls, which is how the decomposition is checked at a third overhead level
# rather than extrapolated to one. Every figure is then computed over the
# tracked subset alone, on both sides.
TRACKN=${TRACKN:-$CLIENTS}
BPIDS_T=$(tr ',' '\n' <<<"$BPIDS" | head -"$TRACKN" | paste -sd, -)
echo "$BPIDS_T" > "$OUT/bpids_track"
NT=$(tr ',' '\n' <<<"$BPIDS_T" | wc -l | tr -d ' ')
[[ "$NT" -eq "$TRACKN" ]] || { echo "FATAL: tracked set is $NT, want $TRACKN"; exit 1; }
echo "tracked backends ($NT of $CLIENTS): $BPIDS_T"

# NODUMP=1 builds the same instrument WITHOUT the per-transition printf, so a
# paired run measures how much the dump itself perturbs the workload. The
# aggregate maps (and therefore the headline shares) are identical code.
python3 - "$BPIDS_T" /root/arb/xtab.bt.tmpl "$OUT/xtab.bt" "${NODUMP:-0}" <<'PY'
import sys, re
bpids, tmpl, out, nodump = sys.argv[1:5]
pids = [p for p in bpids.split(',') if p]
t = open(tmpl).read()
t = t.replace('__TRACK__', '\n'.join('    @track[%s] = 1;' % p for p in pids))
if nodump == "1":
    lines = [l for l in t.split('\n') if 'printf("K %d' not in l]
    t = '\n'.join(lines)
    assert 'printf("K %d' not in t
left = re.findall(r'__[A-Z]+__', t)
assert not left, "unsubstituted placeholders left: %s" % sorted(set(left))
open(out, 'w').write(t)
PY
if [[ "${NODUMP:-0}" == "1" ]]; then
    grep -q 'printf("K %d' "$OUT/xtab.bt" && { echo "FATAL: NODUMP=1 but K printf survived"; exit 1; }
else
    grep -q 'printf("K %d' "$OUT/xtab.bt" || { echo "FATAL: NODUMP=0 but no K printf"; exit 1; }
fi
grep -qE '__[A-Z]+__' "$OUT/xtab.bt" && { echo "FATAL: unsubstituted placeholder"; exit 1; }
NTRACK=$(grep -cE '@track\[[0-9]+\] = 1;' "$OUT/xtab.bt")
[[ "$NTRACK" -eq "$TRACKN" ]] || { echo "FATAL: xtab.bt tracks $NTRACK pids, want $TRACKN"; exit 1; }

BPFTRACE_PERF_RB_PAGES=${RBPAGES:-8192} BPFTRACE_MAP_KEYS_MAX=65536 \
  bpftrace "$OUT/xtab.bt" >"$OUT/sched.txt" 2>"$OUT/sched.err" &
BTPID=$!
for _ in $(seq 1 90); do grep -q '^TS ' "$OUT/sched.txt" 2>/dev/null && break; sleep 1; done
grep -q '^TS ' "$OUT/sched.txt" || { echo "FATAL: bpftrace did not attach"; head -20 "$OUT/sched.err"; exit 1; }
python3 -c 'import time;print(time.clock_gettime_ns(time.CLOCK_MONOTONIC), time.time_ns())' > "$OUT/clockoff"

./pg_wait_tracer --daemon --pid "$PM_PID" -i 1 -T "$TRACE" \
    --mode tiered --sample-rate "$RATE" --escalation-budget 900 -q \
    >/dev/null 2>"$OUT/daemon.log" &
TPID=$!
python3 /root/arb/kernel_cpu_arbiter.py sample --postmaster "$PM_PID" \
    --out "$OUT/kernel.csv" --hz 20 --duration $((ESC + 45)) \
    --extra-pid "$TPID" --extra-pid "$BTPID" >"$OUT/kernel.log" 2>&1 &
KPID=$!
for _ in $(seq 1 40); do [[ -S "$SOCK" ]] && break; sleep 0.3; done
[[ -S "$SOCK" ]] || { echo "FATAL: daemon did not start"; tail -20 "$OUT/daemon.log"; exit 1; }
echo "$TPID" > "$OUT/daemon_pid"; echo "$BTPID" > "$OUT/bt_pid"

python3 -c 'import time;print(time.time_ns())' > "$OUT/esc_from"
python3 - "$SOCK" "{\"cmd\":\"escalate\",\"duration_s\":${ESC},\"reason\":\"xtab\"}" <<'PYEOF'
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

wait "$KPID" 2>/dev/null
wait "$SQLPID" 2>/dev/null
kill -TERM "$TPID" 2>/dev/null; wait "$TPID" 2>/dev/null
kill -TERM "$BTPID" 2>/dev/null; wait "$BTPID" 2>/dev/null
wait "$PGB" 2>/dev/null

psql -U postgres -d postgres -tAF, -c \
  "COPY (SELECT it, (extract(epoch from ts)*1e9)::bigint, pid, state, coalesce(wet,''), coalesce(we,'') FROM pgwt_oracle) TO STDOUT WITH (FORMAT csv)" \
  > "$OUT/oracle.csv" 2>>"$OUT/oracle.log"
psql -U postgres -d postgres -q -c "DROP TABLE IF EXISTS pgwt_oracle; DROP TABLE IF EXISTS pgwt_oracle_meta" >>"$OUT/oracle.log" 2>&1

# bpftrace drops events when the ring buffer overflows and says so on stderr.
# A dropped scheduler transition would silently move time into the wrong cell,
# so this is checked here AND cross-checked against bpftrace's own aggregate
# maps in xtab_join.py.
LOST=$(grep -ciE 'lost|dropped' "$OUT/sched.err" 2>/dev/null)
echo "bpftrace stderr lost/dropped lines: $LOST"
[[ "$LOST" -gt 0 ]] && head -5 "$OUT/sched.err"
echo "sched lines: $(wc -l < "$OUT/sched.txt")  K lines: $(grep -c '^K ' "$OUT/sched.txt")  oracle rows: $(wc -l < "$OUT/oracle.csv")"
echo "tps: $(grep -i 'tps = ' "$OUT/pgbench.log" | head -1)"
echo "XTAB_OK $OUT"
