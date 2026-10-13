#!/usr/bin/env bash
# Paired A/B overhead envelope driver, 4-client unthrottled variant.
# A = pgbench -c 4 (unthrottled) + live_loop_workload.py, no tracer.
# B = same workload with pg_wait_tracer --daemon --mode full attached.
# Interleaved A,B,A,B,A,B. Full logs kept (not tailed).
set -uo pipefail

BASE=/root/overhead-envelope-20261013-4c
SRC="$BASE/src-tree"
TRACER="$SRC/pg_wait_tracer"
LOGDIR="$BASE/logs"
mkdir -p "$LOGDIR"

export PGBENCH=/usr/lib/postgresql/18/bin/pgbench
export PGUSER=postgres
export PGDATABASE=postgres
export PGHOST=127.0.0.1
export PGPORT=5418

DURATION_S=60

PM_PID=$(cat /var/lib/postgresql/18/main/postmaster.pid | head -1)
echo "postmaster pid: $PM_PID"

extract_tps() {
    grep -oP 'tps = \K[\d.]+' "$1" | tail -1
}

run_workload_pair() {
    # $1 = pgbench log path, $2 = workload log path
    "$PGBENCH" -U postgres -d postgres -c 4 -T "$DURATION_S" \
        >"$1" 2>&1 &
    local pgbench_pid=$!

    ( cd "$SRC/tests" && python3 live_loop_workload.py "$DURATION_S" \
        >"$2" 2>&1 ) &
    local workload_pid=$!

    wait "$pgbench_pid"
    local pgbench_rc=$?
    wait "$workload_pid"
    local workload_rc=$?
    echo "pgbench_rc=$pgbench_rc workload_rc=$workload_rc" >>"$2"
}

run_baseline() {
    local n=$1
    local pgbench_log="$LOGDIR/A${n}.pgbench.log"
    local workload_log="$LOGDIR/A${n}.workload.log"
    echo "=== A${n} (baseline, no tracer) start $(date -Iseconds) ===" | tee "$LOGDIR/A${n}.log"
    run_workload_pair "$pgbench_log" "$workload_log"
    echo "=== A${n} end $(date -Iseconds) ===" >>"$LOGDIR/A${n}.log"
    cat "$pgbench_log" >>"$LOGDIR/A${n}.log"
    extract_tps "$pgbench_log"
}

run_with_tracer() {
    local n=$1
    local pgbench_log="$LOGDIR/B${n}.pgbench.log"
    local workload_log="$LOGDIR/B${n}.workload.log"
    local daemon_log="$LOGDIR/B${n}.daemon.log"
    local trace_dir="$BASE/trace_B${n}"
    mkdir -p "$trace_dir"
    echo "=== B${n} (with tracer --mode full) start $(date -Iseconds) ===" | tee "$LOGDIR/B${n}.log"

    "$TRACER" --daemon --pid "$PM_PID" -i 1 -T "$trace_dir" --mode full -v \
        >/dev/null 2>"$daemon_log" &
    local tracer_pid=$!

    local sock="$trace_dir/pgwt.sock"
    local ready=0
    for _ in $(seq 1 60); do
        if [[ -S "$sock" ]]; then ready=1; break; fi
        if ! kill -0 "$tracer_pid" 2>/dev/null; then
            echo "ERROR: daemon exited during startup" | tee -a "$LOGDIR/B${n}.log"
            tail -n 60 "$daemon_log" | tee -a "$LOGDIR/B${n}.log"
            echo ""
            return
        fi
        sleep 0.5
    done
    if [[ "$ready" -ne 1 ]]; then
        echo "ERROR: control socket never appeared at $sock" | tee -a "$LOGDIR/B${n}.log"
        kill "$tracer_pid" 2>/dev/null
        echo ""
        return
    fi
    echo "daemon ready, pid=$tracer_pid, socket=$sock" >>"$LOGDIR/B${n}.log"

    run_workload_pair "$pgbench_log" "$workload_log"

    echo "=== B${n} end $(date -Iseconds) ===" >>"$LOGDIR/B${n}.log"
    cat "$pgbench_log" >>"$LOGDIR/B${n}.log"

    kill "$tracer_pid" 2>/dev/null
    wait "$tracer_pid" 2>/dev/null
    echo "daemon stopped" >>"$LOGDIR/B${n}.log"

    extract_tps "$pgbench_log"
}

RESULTS="$BASE/results.csv"
echo "run,arm,tps" > "$RESULTS"

for n in 1 2 3; do
    tps_a=$(run_baseline "$n")
    echo "$n,A,$tps_a" >> "$RESULTS"
    echo "A${n} tps=$tps_a"

    tps_b=$(run_with_tracer "$n")
    echo "$n,B,$tps_b" >> "$RESULTS"
    echo "B${n} tps=$tps_b"
done

echo "=== results.csv ==="
cat "$RESULTS"
