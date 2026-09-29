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
# Usage: scripts/demo-rehearsal-remote-run.sh DURATION_MIN RUN_MARKER PG_VERSION
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
DURATION_MIN="${1:?Usage: demo-rehearsal-remote-run.sh DURATION_MIN RUN_MARKER PG_VERSION}"
RUN_MARKER="${2:?Usage: demo-rehearsal-remote-run.sh DURATION_MIN RUN_MARKER PG_VERSION}"
PG_VERSION="${3:?Usage: demo-rehearsal-remote-run.sh DURATION_MIN RUN_MARKER PG_VERSION}"

[ -w /tmp/pgwt-box-check.lock ] || { touch /tmp/pgwt-box-check.lock 2>/dev/null && chmod 0666 /tmp/pgwt-box-check.lock 2>/dev/null; }
exec 200>/tmp/pgwt-box-check.lock
flock -x 200

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
    sudo DURATION_MIN="$DURATION_MIN" PGWT_RUN_MARKER="$RUN_MARKER" tests/demo_rehearsal.sh --pg-version "$PG_VERSION"
    rc=$?
fi

echo "$rc" > rehearsal.rc.tmp
mv -f rehearsal.rc.tmp rehearsal.rc
: > rehearsal.done
exit "$rc"
