#!/usr/bin/env bash
# Paired A/B overhead envelope driver for issue: §6 overhead envelope.
# Runs on the gate box as root. A = demo workload, no tracer. B = demo
# workload with pg_wait_tracer --daemon --mode full attached.
# Interleaved A,B,A,B,A,B. Full logs kept (not tailed).
set -uo pipefail

BASE=/root/overhead-envelope-20261013
SRC="$BASE/src-tree"
TRACER="$SRC/pg_wait_tracer"
WORKLOAD="$BASE/workload-242.sh"
LOGDIR="$BASE/logs"
mkdir -p "$LOGDIR"

export PGBENCH=/usr/lib/postgresql/18/bin/pgbench
export PSQL=/usr/lib/postgresql/18/bin/psql
export PGUSER=postgres
export DB=postgres
export PGHOST=127.0.0.1
export PGPORT=5418

PM_PID=$(cat /var/lib/postgresql/18/main/postmaster.pid | head -1)
echo "postmaster pid: $PM_PID"

extract_tps() {
    grep -oP 'tps = \K[\d.]+' "$1" | tail -1
}

run_baseline() {
    local n=$1
    local log="$LOGDIR/A${n}.log"
    echo "=== A${n} (baseline, no tracer) start $(date -Iseconds) ===" | tee "$log"
    bash "$WORKLOAD" >>"$log" 2>&1
    echo "=== A${n} end $(date -Iseconds) ===" >>"$log"
    extract_tps "$log"
}

run_with_tracer() {
    local n=$1
    local log="$LOGDIR/B${n}.log"
    local daemon_log="$LOGDIR/B${n}.daemon.log"
    local trace_dir="$BASE/trace_B${n}"
    mkdir -p "$trace_dir"
    echo "=== B${n} (with tracer --mode full) start $(date -Iseconds) ===" | tee "$log"

    "$TRACER" --daemon --pid "$PM_PID" -i 1 -T "$trace_dir" --mode full -v \
        >/dev/null 2>"$daemon_log" &
    local tracer_pid=$!

    local sock="$trace_dir/pgwt.sock"
    local ready=0
    for _ in $(seq 1 60); do
        if [[ -S "$sock" ]]; then ready=1; break; fi
        if ! kill -0 "$tracer_pid" 2>/dev/null; then
            echo "ERROR: daemon exited during startup" | tee -a "$log"
            tail -n 60 "$daemon_log" | tee -a "$log"
            echo ""
            return
        fi
        sleep 0.5
    done
    if [[ "$ready" -ne 1 ]]; then
        echo "ERROR: control socket never appeared at $sock" | tee -a "$log"
        kill "$tracer_pid" 2>/dev/null
        echo ""
        return
    fi
    echo "daemon ready, pid=$tracer_pid, socket=$sock" >>"$log"

    bash "$WORKLOAD" >>"$log" 2>&1

    echo "=== B${n} end $(date -Iseconds) ===" >>"$log"

    kill "$tracer_pid" 2>/dev/null
    wait "$tracer_pid" 2>/dev/null
    echo "daemon stopped" >>"$log"

    extract_tps "$log"
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
