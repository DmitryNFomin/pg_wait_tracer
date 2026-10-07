#!/usr/bin/env bash
# Runs ON gate-2. Starts ONE capture for ONE walk, on a FRESH EMPTY trace dir
# (PR #295 refuses pre-v4 summary files, and a reused dir would silently fall
# back to the slow raw path -- every number would then be wrong in the
# pessimistic direction). Prints the trace dir and exits; the daemon is
# detached and keeps running. Does NOT wait the 900 s itself, so the caller
# can time the fill and poll conditions.
set -uo pipefail
ROOT=/root/pgwt-paint2
RUN="${1:?usage: box_run_start.sh RUNLABEL}"
cd "$ROOT" || exit 2

TRACE_DIR="/root/paint2_trace_$RUN"
rm -rf "$TRACE_DIR"; mkdir -p "$TRACE_DIR"
[[ -z "$(ls -A "$TRACE_DIR")" ]] || { echo "TRACE DIR NOT EMPTY"; exit 1; }

PM_PID=$(head -1 /var/lib/postgresql/18/main/postmaster.pid)
[[ "$PM_PID" =~ ^[0-9]+$ ]] || { echo "no postmaster pid"; exit 1; }

# Same invocation as tests/ui_live_smoke.sh / tests/demo_rehearsal.sh:
# --mode full -i 1, the demo configuration.
setsid nohup ./pg_wait_tracer --daemon --pid "$PM_PID" -i 1 -T "$TRACE_DIR" \
    --mode full -v > /dev/null 2>"/tmp/paint2_daemon_$RUN.log" < /dev/null &
for _ in $(seq 1 60); do [[ -S "$TRACE_DIR/pgwt.sock" ]] && break; sleep 1; done
[[ -S "$TRACE_DIR/pgwt.sock" ]] || { echo "DAEMON SOCKET NEVER APPEARED"; tail -20 "/tmp/paint2_daemon_$RUN.log"; exit 1; }
DAEMON_PID=$(pgrep -f "pg_wait_tracer --daemon --pid $PM_PID .*paint2_trace_$RUN" | head -1)

# Liveness of the workload is a precondition, not an afterthought: measuring
# paint against an idle system is the trap this whole document warns about.
. /tmp/paint2_pids.env
for p in "$PGBENCH_PID" "$WORKLOAD_PID"; do
    kill -0 "$p" 2>/dev/null || { echo "WORKLOAD PROCESS $p IS GONE -- refusing to start a run"; exit 1; }
done

printf 'RUN=%s\nTRACE_DIR=%s\nDAEMON_PID=%s\nSTART_EPOCH=%s\n' \
    "$RUN" "$TRACE_DIR" "$DAEMON_PID" "$(date +%s)" | tee "/tmp/paint2_run_$RUN.env"
echo "RUN_STARTED"
