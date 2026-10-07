#!/usr/bin/env bash
# Runs ON gate-2, AFTER the 900 s fill and AFTER the walk. Records the
# conditions the paint numbers must be attributed to, so the result can be
# read against volume rather than guessed at:
#   - capture age (wall seconds since the daemon started on the empty dir)
#   - total events written, and the in-window record count for the UI's own
#     15 m window (read from the trace, not inferred from a rate)
#   - box load, workload liveness
set -uo pipefail
ROOT=/root/pgwt-paint2
RUN="${1:?usage: box_run_conditions.sh RUNLABEL}"
cd "$ROOT" || exit 2
. "/tmp/paint2_run_$RUN.env"
. /tmp/paint2_pids.env

NOW=$(date +%s)
echo "run=$RUN capture_age_s=$(( NOW - START_EPOCH ))"
echo "loadavg=$(cut -d' ' -f1-3 /proc/loadavg)"
for p in "$PGBENCH_PID" "$WORKLOAD_PID" "$DAEMON_PID"; do
    if kill -0 "$p" 2>/dev/null; then echo "alive $p: $(ps -p "$p" -o args= | cut -c1-70)";
    else echo "DEAD $p -- this run's numbers are NOT valid"; fi
done
echo "trace dir:"; ls -la "$TRACE_DIR" | tail -n +2
echo "daemon log tail:"; grep -E "events,|records," "/tmp/paint2_daemon_$RUN.log" | tail -3
echo "daemon flush lines:"; tail -4 "/tmp/paint2_daemon_$RUN.log"
echo "--- trace volume (pgwt-server --dump) ---"
# --dump covers the WHOLE trace, not the UI's 900 s window, and is reported as
# such: "Events: N ... Duration: Ds" plus the daemon's own events/s. The
# in-window figure is derived from the rate x 900 s and is labelled derived,
# never presented as a direct count.
./pgwt-server --dump "$TRACE_DIR" 2>/dev/null > "/tmp/paint2_dump_$RUN.txt"
grep -E "^pgwt-server: [0-9]+ trace files|^Daemon: running|^pgwt-server — Summary|^Fidelity:" "/tmp/paint2_dump_$RUN.txt"
grep -E "AAS: |DB Time |^  Idle " "/tmp/paint2_dump_$RUN.txt" | head -4
echo "--- time model (whole trace) ---"
sed -n '/=== Time Model ===/,/=== /p' "/tmp/paint2_dump_$RUN.txt" | head -40
echo "--- distinct sessions in the trace ---"
wc -l < "$TRACE_DIR/backends.jsonl"
echo "--- summary records ---"
grep -cE '.' "$TRACE_DIR/current.summary" 2>/dev/null | head -1 || true
grep -E "summary file closed" "/tmp/paint2_daemon_$RUN.log" | tail -2
