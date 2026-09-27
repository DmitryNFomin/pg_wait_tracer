#!/bin/bash
# bias_capture.sh — one window with FIVE observers, all restricted to the same
# pgbench backends:
#   exact tier + sampled tier (pg_wait_tracer, tiered mode)
#   pg_stat_activity at ~48 Hz from a separate backend
#   /proc schedstat counter deltas
#   sched:sched_switch / sched_wakeup tracepoints, which ALSO record the
#   distribution of "backends on-CPU or runnable" as seen by time vs as seen
#   at the instants the pg_stat_activity sampler is actually scheduled.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
cd "$REPO" || exit 1
source tests/testutil.sh

RATE=${RATE:-50}
ESC=${ESC:-60}
CLIENTS=${CLIENTS:-8}
OUT=${OUT:-/root/arb/bias1}

PM_PID=$(find_postmaster)
PORT=$(awk 'NR==4{print $1}' "$(readlink -f /proc/$PM_PID/cwd)/postmaster.pid" 2>/dev/null)
[[ -n "$PORT" ]] || PORT=5418
export PGPORT="$PORT"
rm -rf "$OUT"; mkdir -p "$OUT/trace"
TRACE="$OUT/trace"; SOCK="$TRACE/pgwt.sock"
echo "postmaster=$PM_PID port=$PGPORT esc=$ESC clients=$CLIENTS out=$OUT nproc=$(nproc)"

pgbench -U postgres -d postgres -c "$CLIENTS" -j 4 -T $((ESC + 40)) >"$OUT/pgbench.log" 2>&1 &
PGB=$!
sleep 6

psql -U postgres -d postgres -q <<'EOF' >"$OUT/setup.log" 2>&1
DROP TABLE IF EXISTS pgwt_oracle; DROP TABLE IF EXISTS pgwt_oracle_meta;
CREATE UNLOGGED TABLE pgwt_oracle(it int, ts timestamptz, pid int, state text, wet text, we text);
CREATE UNLOGGED TABLE pgwt_oracle_meta(k text, v text);
EOF

# The sampler backend, started early so its pid is known before bpftrace
# attaches; only the escalation window is analysed, so its warm-up is ignored.
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
[[ -n "$SAMPLER" ]] || { echo "no sampler pid"; exit 1; }
BPIDS=$(psql -U postgres -d postgres -tAc \
  "SELECT string_agg(pid::text, ',' ORDER BY pid) FROM pg_stat_activity
    WHERE backend_type='client backend' AND application_name='pgbench'" | tr -d '[:space:]')
echo "sampler=$SAMPLER pgbench_backends=$BPIDS"
echo "$SAMPLER" > "$OUT/sampler_pid"; echo "$BPIDS" > "$OUT/bpids"
[[ $(tr ',' '\n' <<<"$BPIDS" | wc -l) -eq $CLIENTS ]] || { echo "backend count mismatch"; exit 1; }

python3 - "$SAMPLER" "$BPIDS" "$HERE/sched2.bt.tmpl" "$OUT/sched2.bt" <<'PY'
import sys
samp, bpids, tmpl, out = sys.argv[1:5]
pids = [p for p in bpids.split(',') if p]
t = open(tmpl).read().replace('__SAMPLER__', samp)
t = t.replace('__TRACK__', '\n'.join('    @track[%s] = 1;' % p for p in pids))
t = t.replace('__SOBS__', '\n'.join(
    '        if (@st[%s] == 1) { @sobs[%s] = count(); }' % (p, p) for p in pids))
open(out, 'w').write(t)
PY
# issue #115 tooling bug: this check was missing here (present in the later
# xtab_capture.sh / pgws_capture.sh) -- a substitution that silently left a
# __PLACEHOLDER__ behind, or tracked the wrong pid count, previously ran
# bpftrace anyway and reported zeros for every counter with no error at all.
grep -qE '__[A-Z]+__' "$OUT/sched2.bt" && { echo "FATAL: unsubstituted placeholder in sched2.bt"; exit 1; }
NTRACK=$(grep -cE '@track\[[0-9]+\] = 1;' "$OUT/sched2.bt")
[[ "$NTRACK" -eq "$CLIENTS" ]] || { echo "FATAL: sched2.bt tracks $NTRACK pids, want $CLIENTS"; exit 1; }
bpftrace "$OUT/sched2.bt" >"$OUT/sched.txt" 2>"$OUT/sched.err" &
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
python3 - "$SOCK" "{\"cmd\":\"escalate\",\"duration_s\":${ESC},\"reason\":\"bias\"}" <<'PYEOF'
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
# The sampler's whole loop is ONE transaction: killing it rolls the samples
# back. Let it finish (it is sized to end just after the escalation window).
wait "$SQLPID" 2>/dev/null
kill -TERM "$TPID" 2>/dev/null; wait "$TPID" 2>/dev/null
kill -TERM "$BTPID" 2>/dev/null; wait "$BTPID" 2>/dev/null
wait "$PGB" 2>/dev/null

psql -U postgres -d postgres -tAF, -c \
  "COPY (SELECT it, (extract(epoch from ts)*1e9)::bigint, pid, state, coalesce(wet,''), coalesce(we,'') FROM pgwt_oracle) TO STDOUT WITH (FORMAT csv)" \
  > "$OUT/oracle.csv" 2>>"$OUT/oracle.log"
psql -U postgres -d postgres -q -c "DROP TABLE IF EXISTS pgwt_oracle; DROP TABLE IF EXISTS pgwt_oracle_meta" >>"$OUT/oracle.log" 2>&1
echo "oracle rows: $(wc -l < "$OUT/oracle.csv")  sched lines: $(wc -l < "$OUT/sched.txt")  kernel rows: $(wc -l < "$OUT/kernel.csv")"
echo "tps: $(grep -i 'tps' "$OUT/pgbench.log" | head -2 | tr '\n' ' ')"
echo "BIAS_OK $OUT"
