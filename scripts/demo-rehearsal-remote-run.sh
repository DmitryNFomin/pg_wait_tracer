#!/usr/bin/env bash
# demo-rehearsal-remote-run.sh -- issue #176. Runs ON the target (a
# throwaway VM, or a persistent box when scripts/demo-rehearsal.sh was
# given PGWT_BOX), started DETACHED by scripts/demo-rehearsal.sh (nohup +
# setsid -w, so it is a child of nothing the launcher's ssh session owns)
# so it survives the launcher's own controlling session -- or the launcher
# process itself -- ending. Does the actual build + the long
# tests/demo_rehearsal.sh capture, then writes a completion marker.
#
# Ordering matters: rehearsal.rc is written FIRST (atomically, via a
# temp-file rename so a reader never sees a partially-written number),
# THEN rehearsal.done. scripts/demo-rehearsal.sh's remote-state probe
# treats "rehearsal.done exists" as "rehearsal.rc is complete and
# readable" -- never check the marker before the rc is safely on disk.
#
# This script IS the long-lived detached process (nohup + setsid -w wrap
# it directly), so it is also where the flock for a SHARED persistent box
# has to live: acquiring it here, before the build even starts, and
# holding it (fd 200 stays open for this whole script's lifetime) is what
# makes a demo-rehearsal on PGWT_BOX queue behind box-check.sh / another
# agent's run on the same box rather than racing it for CPU during a
# timing-sensitive capture. Same lock file box-check.sh uses
# (/tmp/pgwt-box-check.lock) and the same self-heal permission fix (a
# root-created 0644 lock file from an older provision-runner.sh would
# otherwise wedge a non-root caller's flock forever) -- harmless-but-
# unnecessary to also take on a private throwaway VM (nothing else ever
# touches it there), so it is taken unconditionally rather than needing a
# second code path.
#
# Usage: scripts/demo-rehearsal-remote-run.sh DURATION_MIN RUN_MARKER PG_VERSION [RETAIN_TRACE]
# Run from the repo root on the target (the launcher cd's there first).
# Writes, relative to cwd:
#   rehearsal.out    combined stdout/stderr of the whole run
#   rehearsal.rc     the numeric exit code (written before the marker)
#   rehearsal.done   completion marker (written last)
#
# Exit codes written to rehearsal.rc:
#   0    tests/demo_rehearsal.sh itself passed
#   1    preflight failed (box needs re-provisioning) or demo_rehearsal.sh
#        itself failed (see tests/results/demo_rehearsal/summary.json)
#   2    the build (make / make -C tests / make pgwt-client) failed
set -u
DURATION_MIN="${1:?Usage: demo-rehearsal-remote-run.sh DURATION_MIN RUN_MARKER PG_VERSION [RETAIN_TRACE]}"
RUN_MARKER="${2:?Usage: demo-rehearsal-remote-run.sh DURATION_MIN RUN_MARKER PG_VERSION [RETAIN_TRACE]}"
PG_VERSION="${3:?Usage: demo-rehearsal-remote-run.sh DURATION_MIN RUN_MARKER PG_VERSION [RETAIN_TRACE]}"
# Optional 4th arg (docs/DEMO_DELIVERY_QUEUE.md item 2) -- absent (older
# caller) means "0", same as tests/demo_rehearsal.sh's own default.
RETAIN_TRACE="${4:-0}"

[ -w /tmp/pgwt-box-check.lock ] || { touch /tmp/pgwt-box-check.lock 2>/dev/null && chmod 0666 /tmp/pgwt-box-check.lock 2>/dev/null; }
exec 200>/tmp/pgwt-box-check.lock
flock -x 200

# rehearsal.started -- BLOCKER fix (review): the launcher's wait budget
# used to be computed from when THIS SCRIPT WAS LAUNCHED (before the
# flock wait), not from when the capture actually began. On a shared
# persistent box, `flock -x 200` above can queue behind CI or another
# agent for as long as that job takes (CI's own spread on the gate box is
# 29-117 minutes, n=9) -- with a 35-minute rehearsal that margin is
# consumed entirely by queueing alone, so the launcher would declare a
# capture that is running PERFECTLY "stuck" and tear down (well, collect
# and report failure -- a persistent box is never torn down) while it is
# still queued or has only just started. Written atomically (temp +
# rename, same pattern as rehearsal.rc) immediately after the flock
# actually resolves, so scripts/demo-rehearsal.sh's wait-budget
# calculation can be anchored on this instant instead -- see its own
# comment for how it uses this.
date +%s > rehearsal.started.tmp
mv -f rehearsal.started.tmp rehearsal.started

exec >rehearsal.out 2>&1

echo "demo-rehearsal-remote-run: flock acquired, DURATION_MIN=$DURATION_MIN PG_VERSION=$PG_VERSION"

rc=1

if ! bpftool version >/dev/null 2>&1 || [ ! -r /sys/kernel/btf/vmlinux ]; then
    echo "target needs re-provisioning: tests/provision-runner.sh ubuntu did not leave a working bpftool for this kernel"
    rc=1
elif ! { make -j"$(nproc)" && make -C tests && make pgwt-client; }; then
    echo "build failed (make / make -C tests / make pgwt-client)"
    rc=2
else
    # --pg-version: the caller (scripts/demo-rehearsal.sh) already
    # independently confirmed a matching postmaster is running BEFORE
    # starting this detached job (its own PG-version probe) -- passed
    # through here too so the actual capture targets that exact
    # postmaster, not merely "whatever find_postmaster picks with no
    # --pg-version at all" (which is how the port-5413/PG-13 attempt this
    # closes ran uncounted).
    #
    # 200>&-: close the flock fd for this command and everything it forks
    # (tracer, bridge, pgwt-server, pgbench, the lock/sleep workload) --
    # review finding: without this, fd 200 is inherited straight through
    # `sudo` into every child tests/demo_rehearsal.sh starts. Its own
    # cleanup() already detects a leaked child and exits 1 but does NOT
    # kill it (tests/demo_rehearsal.sh, same as tests/ui_live_smoke.sh's
    # documented hazard at its own cleanup()) -- on a SHARED persistent
    # box that leaked child would keep holding /tmp/pgwt-box-check.lock
    # forever, wedging every later box-check and every CI self-hosted step
    # (which waits `-w 7200` on the same lock) until a human intervenes.
    # Harmless when this script only holds the lock for build+capture; the
    # risk is specifically fd inheritance surviving past THIS script's own
    # teardown via an orphaned grandchild.
    sudo DURATION_MIN="$DURATION_MIN" PGWT_RUN_MARKER="$RUN_MARKER" RETAIN_TRACE="$RETAIN_TRACE" tests/demo_rehearsal.sh --pg-version "$PG_VERSION" 200>&-
    rc=$?
fi

echo "$rc" > rehearsal.rc.tmp
mv -f rehearsal.rc.tmp rehearsal.rc
: > rehearsal.done
exit "$rc"
