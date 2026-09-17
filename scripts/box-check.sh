#!/usr/bin/env bash
# box-check.sh — run the LIVE tier (Linux build + C units + synthetic + protocol
# drift + real-PostgreSQL capture) on an x86 Linux box over ssh. Nothing eBPF
# runs on the Mac; this is the definition of done for any capture change.
#
#   make box-check                 # default box ($PGWT_BOX), all PG versions
#   make box-check PG=13           # one PG major (--pg-version)
#   make box-check OS=el8          # $PGWT_BOX_EL8 (Rocky 8 / kernel 4.18)
#
# Env:
#   PGWT_BOX          ssh target for the default (Ubuntu) box, e.g. root@1.2.3.4
#   PGWT_BOX_EL8 / PGWT_BOX_EL9 / PGWT_BOX_UBUNTU   per-OS targets
#   PGWT_BOX_DIR      remote checkout root (default ~/pgwt-check)
#   PGWT_PGPORT / PGWT_PG_PORT_BASE   forwarded to the remote tests/run_all.sh
#                     (via `sudo env ...`, since sudo's default env_reset would
#                     otherwise drop them) when set on the Mac side; see
#                     tests/run_all.sh and tests/ui_live_smoke.sh for what
#                     each one overrides
#
# Each run gets its own remote directory named after the local branch, and a
# box-wide flock serialises concurrent runs (timing tests must not overlap).
# The full log lands in tests/results/box-check-<os>-<ts>.log; the last lines
# (run_all.sh summary) are echoed so an agent can paste them into a PR.
set -uo pipefail
cd "$(dirname "$0")/.."

OS="${OS:-ubuntu}"
PG="${PG:-}"
case "$OS" in
    ubuntu) target="${PGWT_BOX_UBUNTU:-${PGWT_BOX:-}}" ;;
    el8)    target="${PGWT_BOX_EL8:-}" ;;
    el9)    target="${PGWT_BOX_EL9:-}" ;;
    *) echo "OS must be ubuntu|el8|el9"; exit 2 ;;
esac
if [[ -z "$target" ]]; then
    echo "no box for OS=$OS: set PGWT_BOX (ubuntu) or PGWT_BOX_EL8/EL9 to an ssh target." >&2
    echo "Provisioning: tests/provision-runner.sh (step 4 of the dev-loop plan) / tests/hetzner-vm.sh" >&2
    exit 2
fi

# issue #93: this invocation's start time, used below to tell a FRESH
# tests/results/ui_live/run.id (written by run_all.sh's own $PGWT_RUN_MARKER,
# which is always >= this) from a STALE one left over from a previous
# box-check (tests/results is excluded from the up-rsync above, so the box
# keeps the last run's ui_live/ between invocations).
box_check_start_epoch=$(date +%s)

branch=$(git rev-parse --abbrev-ref HEAD | tr '/' '_')
remote_root="${PGWT_BOX_DIR:-pgwt-check}"
remote_dir="$remote_root/$branch"
ts=$(date +%Y%m%d-%H%M%S)
mkdir -p tests/results
log="tests/results/box-check-$OS-$ts.log"

echo "box-check: $target  OS=$OS PG=${PG:-all}  -> $remote_dir  (log: $log)"
ssh -o BatchMode=yes "$target" "mkdir -p '$remote_dir'" || exit 1
# The binary excludes are anchored to the repo root (leading /): an
# unanchored 'pg_wait_tracer*' also matches src/pg_wait_tracer.c,
# src/pg_wait_tracer.h and src/bpf/pg_wait_tracer.bpf.c (rsync excludes
# without a '/' match at any depth), which silently dropped the daemon's
# own source from every box-check rsync and made the remote build fail
# with "No rule to make target 'build/pg_wait_tracer.o'".
rsync -az --delete \
    --exclude .git --exclude /build \
    --exclude '/pgwt-server' --exclude '/pgwt-server-asan' \
    --exclude '/pg_wait_tracer' --exclude '/pg_wait_tracer-asan' \
    --exclude 'tests/results' --exclude 'web/pgwt' --exclude '__pycache__' \
    --exclude '.pgwt-check.stamp' \
    ./ "$target:$remote_dir/" || exit 1

pgarg=""; [[ -n "$PG" ]] && pgarg="--pg-version $PG"
# The remote command below runs tests/run_all.sh via sudo, and default
# sudoers policy resets the environment (env_reset) -- so PGWT_PGPORT /
# PGWT_PG_PORT_BASE, documented overrides that tests/run_all.sh and
# tests/ui_live_smoke.sh both read directly, never reached run_all.sh even
# when a caller exported them before `make box-check`. Forward only the ones
# actually set on the Mac side, explicitly, via `sudo env VAR=val ...` (sudo
# itself, not the shell inheriting an already-reset environment).
sudo_env=""
[[ -n "${PGWT_PGPORT:-}" ]] && sudo_env="$sudo_env PGWT_PGPORT=$PGWT_PGPORT"
[[ -n "${PGWT_PG_PORT_BASE:-}" ]] && sudo_env="$sudo_env PGWT_PG_PORT_BASE=$PGWT_PG_PORT_BASE"
# Preflight (runs on the box, inside the same flock, before `make`): a
# kernel change without re-provisioning left bpftool's dispatcher unable to
# find a matching linux-tools package ("bpftool not found for kernel
# 6.8.0-139") -- the vmlinux.h generation step then died with an opaque
# Makefile error. Fail loud and self-explaining here instead. Tests the
# REAL failure mode only: `bpftool version` works AND the running kernel's
# BTF is readable. NOT a `linux-tools-$(uname -r)` package-name check --
# tests/provision-runner.sh deliberately falls back to linux-tools-generic
# or a source-built /usr/local/bin/bpftool when the exact-version package
# is unavailable, so that package's mere absence does not mean anything is
# actually broken.
preflight="if ! bpftool version >/dev/null 2>&1 || [ ! -r /sys/kernel/btf/vmlinux ]"
preflight="$preflight; then echo \"gate box needs re-provisioning after a kernel change: run tests/provision-runner.sh ubuntu on the box\"; exit 2; fi;"
# shellcheck disable=SC2029
# make pgwt-client: builds web/pgwt (the Go bridge) -- `make` alone (the
# `all` target) only builds the daemon + pgwt-server. Not needed until
# issue #93's live-UI-smoke test (tests/ui_live_smoke.sh checks for
# web/pgwt and fails loudly if it is missing, same as the other binaries).
ssh -o BatchMode=yes "$target" \
    "cd '$remote_dir' && flock /tmp/pgwt-box-check.lock bash -c '$preflight make -j\$(nproc) && make -C tests && make pgwt-client && sudo env$sudo_env tests/run_all.sh --require-live $pgarg'" \
    2>&1 | tee "$log"
rc=${PIPESTATUS[0]}

# issue #93: pull the live-UI-smoke artifacts back regardless of rc — a
# failing run's frames/video are exactly what a reviewer needs (a `-n` skip
# vs. FAIL, and which tab/tick broke). Unconditional so this never masks
# run_all.sh's own exit code; a missing remote dir (test skipped before
# producing anything) is reported, not fatal. Honours the same run.id
# marker run_all.sh's own re-emit gates on (review item 3): a stale ui_live/
# left over from a previous box-check must never be reported as if it were
# this run's evidence.
mkdir -p tests/results/ui_live
if rsync -az --delete "$target:$remote_dir/tests/results/ui_live/" tests/results/ui_live/ 2>/dev/null; then
    run_id="$(cat tests/results/ui_live/run.id 2>/dev/null || true)"
    if [[ -n "$run_id" && "$run_id" =~ ^[0-9]+$ && "$run_id" -ge "$box_check_start_epoch" ]]; then
        echo "box-check: tests/results/ui_live/ synced back from $target (run.id=$run_id, this invocation)"
    else
        echo "box-check: tests/results/ui_live/ synced back from $target BUT run.id=${run_id:-<missing>} predates this invocation (started $box_check_start_epoch) -- STALE evidence from a previous run, the smoke likely did not complete this time"
    fi
else
    echo "box-check: no tests/results/ui_live/ on $target (test_ui_live_smoke skipped or produced no artifacts)"
fi

echo
echo "box-check ($OS, PG=${PG:-all}) exit=$rc — summary:"
tail -n 25 "$log" | sed 's/^/  /'
exit "$rc"
