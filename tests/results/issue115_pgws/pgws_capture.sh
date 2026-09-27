#!/bin/bash
# pgws_capture.sh — issue #115 follow-up: does pg_wait_sampling carry the same
# scheduler-induced sampling bias?  One window, SIX observers, all restricted
# to the same pgbench backends:
#
#   exact tier + sampled tier (pg_wait_tracer, tiered mode)
#   pg_stat_activity at ~48 Hz from a separate backend
#   pg_wait_sampling's profile (its collector bgworker at profile_period=10ms)
#   /proc schedstat counter deltas
#   sched:sched_switch / sched_wakeup tracepoints, which ALSO record the
#   run-queue state at the instants each of the TWO in-database samplers
#   (the pgsa poller and the pgws collector) was actually switched in.
#
# Derived from tests/results/issue115_reconcile/recon_capture.sh; the only
# additions are the pg_wait_sampling snapshotter, the collector pid, and
# sched3.bt.tmpl in place of sched2.bt.tmpl.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
cd "$REPO" || exit 1
source tests/testutil.sh

RATE=${RATE:-50}
ESC=${ESC:-60}
CLIENTS=${CLIENTS:-8}
OUT=${OUT:-/root/arb/pgws1}
PGWS_HZ=${PGWS_HZ:-2}

PM_PID=$(find_postmaster)
PORT=$(awk 'NR==4{print $1}' "$(readlink -f /proc/$PM_PID/cwd)/postmaster.pid" 2>/dev/null)
[[ -n "$PORT" ]] || PORT=5418
export PGPORT="$PORT"
rm -rf "$OUT"; mkdir -p "$OUT/trace"
TRACE="$OUT/trace"; SOCK="$TRACE/pgwt.sock"

# A previous run that failed a guard can leave pgbench, the samplers or
# bpftrace behind; their backends would then be picked up as this run's
# and silently widen the measured population. Clear them first.
pkill -f 'pgbench -U postgres' >/dev/null 2>&1
pkill -f 'psql .*sampler\.sql' >/dev/null 2>&1
pkill -f 'psql .*pgws\.sql' >/dev/null 2>&1
pkill -x bpftrace >/dev/null 2>&1
pkill -f 'pg_wait_tracer --daemon' >/dev/null 2>&1
sleep 2
psql -U postgres -d postgres -tAc \
  "SELECT pg_terminate_backend(pid) FROM pg_stat_activity
    WHERE backend_type='client backend' AND pid <> pg_backend_pid()
      AND coalesce(application_name,'') IN ('pgbench','pgws_snap','psql')" \
  >/dev/null 2>&1
sleep 1

# The pg_wait_sampling collector: a background worker of THIS postmaster.
COLL=$(pgrep -P "$PM_PID" -f 'pg_wait_sampling collector' | head -1)
[[ -n "$COLL" ]] || { echo "FATAL: no pg_wait_sampling collector under postmaster $PM_PID"; exit 1; }
echo "postmaster=$PM_PID port=$PGPORT esc=$ESC clients=$CLIENTS out=$OUT nproc=$(nproc) collector=$COLL"
echo "$COLL" > "$OUT/collector_pid"
psql -U postgres -d postgres -tAc \
  "select name||'='||setting from pg_settings where name like 'pg_wait_sampling%'" \
  > "$OUT/pgws_settings" 2>&1
cat "$OUT/pgws_settings"

pgbench -U postgres -d postgres -c "$CLIENTS" -j 4 -T $((ESC + 40)) >"$OUT/pgbench.log" 2>&1 &
PGB=$!
sleep 6

psql -U postgres -d postgres -q <<'EOF' >"$OUT/setup.log" 2>&1
DROP TABLE IF EXISTS pgwt_oracle; DROP TABLE IF EXISTS pgwt_oracle_meta;
DROP TABLE IF EXISTS pgws_snap;
CREATE UNLOGGED TABLE pgwt_oracle(it int, ts timestamptz, pid int, state text, wet text, we text);
CREATE UNLOGGED TABLE pgwt_oracle_meta(k text, v text);
CREATE UNLOGGED TABLE pgws_snap(ts timestamptz, pid int, wet text, we text, cnt bigint);
EOF

# --- observer: pg_stat_activity poller (unchanged from recon_capture.sh) ---
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
       WHERE backend_type = 'client backend' AND pid <> pg_backend_pid()
         AND coalesce(application_name,'') <> 'pgws_snap';
    PERFORM pg_sleep(0.02);
  END LOOP;
END \$\$;
EOF
psql -U postgres -d postgres -q -f "$OUT/sampler.sql" >"$OUT/oracle.log" 2>&1 &
SQLPID=$!

# --- observer: pg_wait_sampling profile snapshotter ---------------------
# pg_wait_sampling_profile holds CUMULATIVE per (pid, queryid, event) counts,
# so the window's counts are a difference of two snapshots.  Each read does a
# shm_mq round trip that wakes the collector out of band; at PGWS_HZ=2 that is
# 2 extra wakeups/s against the ~100/s the profile period already causes, and
# the run reports both rates so the perturbation is visible rather than
# assumed.  Snapshots run to a fixed cadence so the analysis window can be
# clamped to the same interval every other observer covers.
cat > "$OUT/pgws.sql" <<EOF
SET application_name = 'pgws_snap';
INSERT INTO pgwt_oracle_meta VALUES ('pgws_snap_pid', pg_backend_pid()::text);
DO \$\$
DECLARE t_end timestamptz := clock_timestamp() + interval '$((ESC + 25)) seconds';
        tick timestamptz;
BEGIN
  LOOP
    EXIT WHEN clock_timestamp() > t_end;
    tick := clock_timestamp();
    INSERT INTO pgws_snap
      SELECT tick, pid, coalesce(event_type,''), coalesce(event,''), sum(count)
        FROM pg_wait_sampling_profile GROUP BY 1,2,3,4;
    COMMIT;
    PERFORM pg_sleep($(python3 -c "print(1.0/$PGWS_HZ)"));
  END LOOP;
END \$\$;
EOF
psql -U postgres -d postgres -q -f "$OUT/pgws.sql" >"$OUT/pgws.log" 2>&1 &
PGWSPID=$!

for _ in $(seq 1 50); do
    SAMPLER=$(psql -U postgres -d postgres -tAc "SELECT v FROM pgwt_oracle_meta WHERE k='sampler_pid'" 2>/dev/null | tr -d '[:space:]')
    SNAP=$(psql -U postgres -d postgres -tAc "SELECT v FROM pgwt_oracle_meta WHERE k='pgws_snap_pid'" 2>/dev/null | tr -d '[:space:]')
    [[ -n "$SAMPLER" && -n "$SNAP" ]] && break
    sleep 0.3
done
[[ -n "$SAMPLER" && -n "$SNAP" ]] || { echo "no sampler/snapshotter pid"; exit 1; }
BPIDS=$(psql -U postgres -d postgres -tAc \
  "SELECT string_agg(pid::text, ',' ORDER BY pid) FROM pg_stat_activity
    WHERE backend_type='client backend' AND application_name='pgbench'" | tr -d '[:space:]')
echo "sampler=$SAMPLER pgws_snap=$SNAP collector=$COLL pgbench_backends=$BPIDS"
echo "$SAMPLER" > "$OUT/sampler_pid"; echo "$BPIDS" > "$OUT/bpids"
echo "$SNAP" > "$OUT/snap_pid"
[[ $(tr ',' '\n' <<<"$BPIDS" | wc -l) -eq $CLIENTS ]] || { echo "backend count mismatch"; exit 1; }

python3 - "$SAMPLER" "$COLL" "$BPIDS" "$HERE/sched3.bt.tmpl" "$OUT/sched3.bt" <<'PY'
import sys
samp, coll, bpids, tmpl, out = sys.argv[1:6]
pids = [p for p in bpids.split(',') if p]
t = open(tmpl).read().replace('__SAMPLER__', samp).replace('__COLLECTOR__', coll)
t = t.replace('__TRACK__', '\n'.join('    @track[%s] = 1;' % p for p in pids))
for marker, m in (('__SOBS__', 'sobs'), ('__COBS__', 'cobs'), ('__COBSW__', 'cobsw')):
    t = t.replace(marker, '\n'.join(
        '        if (@st[%s] == 1) { @%s[%s] = count(); }' % (p, m, p) for p in pids))
import re
left = re.findall(r'__[A-Z]+__', t)
assert not left, "unsubstituted placeholders left: %s" % sorted(set(left))
open(out, 'w').write(t)
PY
# The committed arbiter sched2.bt shipped with one run's pids already baked in
# and then silently reported zeros (issue #115's "tooling gotcha"). Fail loud
# instead: no placeholder may survive, and the tracked set must be non-empty.
if grep -qE '__[A-Z]+__' "$OUT/sched3.bt"; then
    echo "FATAL: unsubstituted placeholder in sched3.bt"; exit 1
fi
NTRACK=$(grep -cE '@track\[[0-9]+\] = 1;' "$OUT/sched3.bt")
[[ "$NTRACK" -eq "$CLIENTS" ]] || { echo "FATAL: sched3.bt tracks $NTRACK pids, want $CLIENTS"; exit 1; }
bpftrace "$OUT/sched3.bt" >"$OUT/sched.txt" 2>"$OUT/sched.err" &
BTPID=$!
for _ in $(seq 1 90); do grep -q '^TS ' "$OUT/sched.txt" 2>/dev/null && break; sleep 1; done
grep -q '^TS ' "$OUT/sched.txt" || { echo "bpftrace did not attach"; head -20 "$OUT/sched.err"; exit 1; }
python3 -c 'import time;print(time.clock_gettime_ns(time.CLOCK_MONOTONIC), time.time_ns())' > "$OUT/clockoff"

./pg_wait_tracer --daemon --pid "$PM_PID" -i 1 -T "$TRACE" \
    --mode tiered --sample-rate "$RATE" --escalation-budget 900 -q \
    >/dev/null 2>"$OUT/daemon.log" &
TPID=$!
python3 "$REPO/tests/kernel_cpu_arbiter.py" sample --postmaster "$PM_PID" \
    --out "$OUT/kernel.csv" --hz 20 --duration $((ESC + 45)) \
    --extra-pid "$TPID" --extra-pid "$BTPID" >"$OUT/kernel.log" 2>&1 &
KPID=$!
for _ in $(seq 1 40); do [[ -S "$SOCK" ]] && break; sleep 0.3; done
[[ -S "$SOCK" ]] || { echo "daemon did not start"; tail -20 "$OUT/daemon.log"; exit 1; }
echo "$TPID" > "$OUT/daemon_pid"; echo "$BTPID" > "$OUT/bt_pid"

python3 -c 'import time;print(time.time_ns())' > "$OUT/esc_from"
python3 - "$SOCK" "{\"cmd\":\"escalate\",\"duration_s\":${ESC},\"reason\":\"pgws\"}" <<'PYEOF'
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
wait "$PGWSPID" 2>/dev/null
kill -TERM "$TPID" 2>/dev/null; wait "$TPID" 2>/dev/null
kill -TERM "$BTPID" 2>/dev/null; wait "$BTPID" 2>/dev/null
wait "$PGB" 2>/dev/null

psql -U postgres -d postgres -tAF, -c \
  "COPY (SELECT it, (extract(epoch from ts)*1e9)::bigint, pid, state, coalesce(wet,''), coalesce(we,'') FROM pgwt_oracle) TO STDOUT WITH (FORMAT csv)" \
  > "$OUT/oracle.csv" 2>>"$OUT/oracle.log"
psql -U postgres -d postgres -tAF, -c \
  "COPY (SELECT (extract(epoch from ts)*1e9)::bigint, pid, wet, we, cnt FROM pgws_snap) TO STDOUT WITH (FORMAT csv)" \
  > "$OUT/pgws.csv" 2>>"$OUT/pgws.log"
psql -U postgres -d postgres -q -c "DROP TABLE IF EXISTS pgwt_oracle; DROP TABLE IF EXISTS pgwt_oracle_meta; DROP TABLE IF EXISTS pgws_snap" >>"$OUT/oracle.log" 2>&1
echo "oracle rows: $(wc -l < "$OUT/oracle.csv")  pgws rows: $(wc -l < "$OUT/pgws.csv")  sched lines: $(wc -l < "$OUT/sched.txt")  kernel rows: $(wc -l < "$OUT/kernel.csv")"
echo "tps: $(grep -i 'tps' "$OUT/pgbench.log" | head -2 | tr '\n' ' ')"
echo "PGWS_OK $OUT"
