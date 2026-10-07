#!/usr/bin/env bash
# Runs the remaining runs back to back. Each gets a FRESH EMPTY trace dir and
# its own 900 s fill (the pinned n=4 was four independent captures, not four
# walks over one capture; a single long capture would also grow the trace file
# monotonically and confound volume with capture age). The workload and pgbench
# keep running across all runs, so the load itself is identical.
#
# Sequential on purpose: a second --mode full daemon attached to the same
# postmaster would double capture overhead on a 4-core box and contaminate the
# run it was overlapping with.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BOX=root@142.132.191.18
for RUN in "$@"; do
    echo "######## $RUN: starting a fresh capture ########"
    date -u
    ssh -o ConnectTimeout=10 "$BOX" "bash /root/pgwt-paint2/box_run_start.sh $RUN" || exit 1
    bash "$HERE/mac_run.sh" "$RUN" || echo "$RUN: mac_run returned non-zero"
    echo "######## $RUN: stopping its daemon ########"
    ssh -o ConnectTimeout=10 "$BOX" \
        ". /tmp/paint2_run_$RUN.env; kill \$DAEMON_PID 2>/dev/null; sleep 6; \
         kill -0 \$DAEMON_PID 2>/dev/null && echo 'daemon still up' || echo 'daemon stopped'; \
         tail -3 /tmp/paint2_daemon_$RUN.log"
done
echo "CHAIN_DONE"
