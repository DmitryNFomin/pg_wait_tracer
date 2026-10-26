#!/bin/bash
# arm261.sh — INSTRUMENT ONLY (#261 / #306). One cross-validation arm that
# PRESERVES the trace directory, so the sampled and exact tiers can be
# re-compared offline at the sampler's own tick instants.
#
# Mirrors tests/test_cross_validate_tiered.sh's single-rate arm. Differences,
# all of them deliberate:
#   * the trace dir is kept (the product test deletes it);
#   * perf throttling observables are captured around the window
#     (perf_event_max_sample_rate, a kprobe count of perf_log_throttle, the
#     NMI-duration dmesg lines) — #306's candidate mechanism;
#   * per-backend /proc/<pid>/schedstat is snapshotted at window open/close,
#     giving an on-CPU reference independent of BOTH tiers;
#   * the daemon's scheduling policy and the kernel's max sample rate are
#     settable, so the #306 arms are reproducible from one script.
#
# Usage: arm261.sh OUTDIR [--rate 10] [--clients 8] [--esc 60]
#                  [--fifo] [--perf-rate N] [--port 5418]
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
TRACER="$ROOT/pg_wait_tracer"

OUT="${1:?usage: arm261.sh OUTDIR [opts]}"; shift
RATE=10; CLIENTS=8; ESC=60; FIFO=0; PERF_RATE=""; PORT=5418
while [[ $# -gt 0 ]]; do
  case "$1" in
    --rate) RATE="$2"; shift 2 ;;
    --clients) CLIENTS="$2"; shift 2 ;;
    --esc) ESC="$2"; shift 2 ;;
    --fifo) FIFO=1; shift ;;
    --perf-rate) PERF_RATE="$2"; shift 2 ;;
    --port) PORT="$2"; shift 2 ;;
    *) echo "unknown arg $1" >&2; exit 2 ;;
  esac
done
mkdir -p "$OUT"
TRACE="$OUT/trace"; mkdir -p "$TRACE"
SOCK="$TRACE/pgwt.sock"
PSQL="psql -h /var/run/postgresql -p $PORT -U postgres -d postgres"

PM_PID=$($PSQL -tAc "SELECT pg_backend_pid()" >/dev/null 2>&1 && \
         pgrep -f "postgres.*-D /var/lib/postgresql/18/main" | head -1)
[[ -z "$PM_PID" ]] && { echo "ERROR: no postmaster on port $PORT" >&2; exit 1; }
echo "postmaster=$PM_PID port=$PORT rate=${RATE}Hz clients=$CLIENTS esc=${ESC}s fifo=$FIFO perf_rate=${PERF_RATE:-unchanged}" | tee "$OUT/arm.txt"

# ── #306 observables, BEFORE ────────────────────────────────────────────
PERF_RATE_ORIG=$(cat /proc/sys/kernel/perf_event_max_sample_rate)
echo "perf_event_max_sample_rate_before=$PERF_RATE_ORIG" >> "$OUT/arm.txt"
if [[ -n "$PERF_RATE" ]]; then
    sysctl -w kernel.perf_event_max_sample_rate="$PERF_RATE" >/dev/null
    echo "perf_event_max_sample_rate_set=$(cat /proc/sys/kernel/perf_event_max_sample_rate)" >> "$OUT/arm.txt"
fi
dmesg | grep -c "interrupt took too long" > "$OUT/nmi_long_before.txt" 2>/dev/null || echo 0 > "$OUT/nmi_long_before.txt"

# Direct count of perf throttle/unthrottle decisions. perf_log_throttle() is
# called with (event, enable=0) when the kernel throttles an event and
# (event, 1) when it unthrottles — the ONLY in-kernel signal that a
# breakpoint event was stopped, since PERF_RECORD_THROTTLE goes to a ring
# buffer this daemon never maps and hw.state is not reflected in
# PERF_FORMAT_TOTAL_TIME_RUNNING.
BT_PID=""
if command -v bpftrace >/dev/null 2>&1; then
    nohup bpftrace -e 'kprobe:perf_log_throttle { @throttle[arg1] = count(); }
                       interval:s:3600 { exit(); }' \
        > "$OUT/throttle.txt" 2> "$OUT/throttle.err" &
    BT_PID=$!
    sleep 4
    echo "bpftrace_pid=$BT_PID" >> "$OUT/arm.txt"
else
    echo "bpftrace=MISSING — throttle counting UNAVAILABLE (this arm cannot answer #306)" | tee -a "$OUT/arm.txt"
fi

# ── daemon ──────────────────────────────────────────────────────────────
PRE=()
[[ "$FIFO" -eq 1 ]] && PRE=(chrt -f 10)
"${PRE[@]}" "$TRACER" --daemon --pid "$PM_PID" -i 1 -T "$TRACE" \
    --mode tiered --sample-rate "$RATE" --escalation-budget 600 -q \
    >"$OUT/daemon.out" 2>"$OUT/daemon.err" &
TPID=$!
for _ in $(seq 1 40); do [[ -S "$SOCK" ]] && break; sleep 0.3; done
[[ -S "$SOCK" ]] || { echo "ERROR: daemon never created $SOCK" >&2; tail -20 "$OUT/daemon.err"; kill "$TPID" 2>/dev/null; exit 1; }
awk '{print "daemon_sched_policy_field=" $41}' /proc/"$TPID"/stat >> "$OUT/arm.txt"

ctl() {
  python3 - "$SOCK" "$1" <<'PY'
import socket,sys
s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM); s.settimeout(10)
s.connect(sys.argv[1]); s.sendall((sys.argv[2]+"\n").encode())
buf=b""
while b"\n" not in buf:
    c=s.recv(65536)
    if not c: break
    buf+=c
sys.stdout.write(buf.decode().split("\n")[0])
PY
}

pgbench -h /var/run/postgresql -p "$PORT" -U postgres -d postgres \
    -c "$CLIENTS" -j 2 -T $((ESC + 10)) >"$OUT/pgbench.txt" 2>&1 &
PGB=$!
sleep 3

ctl '{"cmd":"metrics"}' > "$OUT/metrics_pre.json"; echo >> "$OUT/metrics_pre.json"
RESP=$(ctl "{\"cmd\":\"escalate\",\"duration_s\":${ESC},\"reason\":\"issue261\"}")
echo "escalate: $RESP" | tee -a "$OUT/arm.txt"
# Backend-independent on-CPU reference, window open.
for p in $(pgrep -P "$PM_PID"); do
    printf '%s\t%s\n' "$p" "$(cat /proc/$p/schedstat 2>/dev/null)"
done > "$OUT/schedstat_open.tsv"
date +%s.%N > "$OUT/t_open.txt"

sleep "$ESC"
date +%s.%N > "$OUT/t_close.txt"
for p in $(pgrep -P "$PM_PID"); do
    printf '%s\t%s\n' "$p" "$(cat /proc/$p/schedstat 2>/dev/null)"
done > "$OUT/schedstat_close.tsv"
ctl '{"cmd":"metrics"}' > "$OUT/metrics_post.json"; echo >> "$OUT/metrics_post.json"
sleep 3

kill "$PGB" 2>/dev/null; wait "$PGB" 2>/dev/null
kill -TERM "$TPID" 2>/dev/null; wait "$TPID" 2>/dev/null

# ── #306 observables, AFTER ─────────────────────────────────────────────
if [[ -n "$BT_PID" ]]; then kill -TERM "$BT_PID" 2>/dev/null; wait "$BT_PID" 2>/dev/null; fi
dmesg | grep -c "interrupt took too long" > "$OUT/nmi_long_after.txt" 2>/dev/null || echo 0 > "$OUT/nmi_long_after.txt"
echo "perf_event_max_sample_rate_after=$(cat /proc/sys/kernel/perf_event_max_sample_rate)" >> "$OUT/arm.txt"
if [[ -n "$PERF_RATE" ]]; then
    sysctl -w kernel.perf_event_max_sample_rate="$PERF_RATE_ORIG" >/dev/null
    echo "perf_event_max_sample_rate_restored=$(cat /proc/sys/kernel/perf_event_max_sample_rate)" >> "$OUT/arm.txt"
fi

"$ROOT/tests/cross_validate" "$TRACE" --tolerance 8.8 --raw-ns --show-idle \
    > "$OUT/xval.txt" 2> "$OUT/xval.err"; echo "xval_rc=$?" >> "$OUT/arm.txt"
"$ROOT/tests/dump_raw_events" "$TRACE" > "$OUT/events.tsv" 2> "$OUT/dump.err"
echo "events_tsv_lines=$(wc -l < "$OUT/events.tsv")" >> "$OUT/arm.txt"
echo "=== arm done: $OUT ==="
