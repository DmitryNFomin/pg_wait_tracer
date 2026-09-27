#!/usr/bin/env bash
# demo-rehearsal-remote-run.sh -- issue #176. Runs ON the throwaway VM,
# started DETACHED by scripts/demo-rehearsal.sh (nohup + setsid -w, so it
# is a child of nothing the launcher's ssh session owns) so it survives
# the launcher's own controlling session -- or the launcher process itself
# -- ending. Does the actual build + the long tests/demo_rehearsal.sh
# capture, then writes a completion marker.
#
# Ordering matters: rehearsal.rc is written FIRST (atomically, via a
# temp-file rename so a reader never sees a partially-written number),
# THEN rehearsal.done. scripts/demo-rehearsal.sh's remote-state probe
# treats "rehearsal.done exists" as "rehearsal.rc is complete and
# readable" -- never check the marker before the rc is safely on disk.
#
# Usage: scripts/demo-rehearsal-remote-run.sh DURATION_MIN RUN_MARKER
# Run from the repo root on the VM (the launcher cd's there first). Writes,
# relative to cwd:
#   rehearsal.out    combined stdout/stderr of the whole run
#   rehearsal.rc     the numeric exit code (written before the marker)
#   rehearsal.done   completion marker (written last)
#
# Exit codes written to rehearsal.rc:
#   0    tests/demo_rehearsal.sh itself passed
#   1    preflight failed (VM needs re-provisioning) or demo_rehearsal.sh
#        itself failed (see tests/results/demo_rehearsal/summary.json)
#   2    the build (make / make -C tests / make pgwt-client) failed
set -u
DURATION_MIN="${1:?Usage: demo-rehearsal-remote-run.sh DURATION_MIN RUN_MARKER}"
RUN_MARKER="${2:?Usage: demo-rehearsal-remote-run.sh DURATION_MIN RUN_MARKER}"

exec >rehearsal.out 2>&1

rc=1

if ! bpftool version >/dev/null 2>&1 || [ ! -r /sys/kernel/btf/vmlinux ]; then
    echo "ephemeral VM needs re-provisioning: tests/provision-runner.sh ubuntu did not leave a working bpftool for this kernel"
    rc=1
elif ! { make -j"$(nproc)" && make -C tests && make pgwt-client; }; then
    echo "build failed (make / make -C tests / make pgwt-client)"
    rc=2
else
    sudo DURATION_MIN="$DURATION_MIN" PGWT_RUN_MARKER="$RUN_MARKER" tests/demo_rehearsal.sh
    rc=$?
fi

echo "$rc" > rehearsal.rc.tmp
mv -f rehearsal.rc.tmp rehearsal.rc
: > rehearsal.done
exit "$rc"
