#!/bin/bash
# demo_rehearsal.sh -- issue #157: "demo-rehearsal" harness. A single, long
# (default 35 min, DURATION_MIN=) --mode full capture against a real
# PostgreSQL under continuous pgbench + lock/sleep load, walked repeatedly
# (not once) across the whole window, plus three end-of-capture checks
# (time-model conservation, the Waterfall executions query's latency, a
# clean daemon log). This is the instrument that decides whether the owner
# can demo -- see docs/ROADMAP_AND_STATUS.md and CLAUDE.md.
#
# EXTENDS, does not fork, tests/ui_live_smoke.sh (CLAUDE.md): the daemon /
# bridge / pgbench / lock-sleep-workload setup below is the same sequence,
# sharing tests/live_daemon_lib.sh (PGPORT derivation, process teardown,
# readiness polling) and tests/live_loop_workload.py (the looping lock/
# sleep workload) so the two scripts cannot drift apart. What's genuinely
# different is the walk itself (tests/demo_rehearsal.py repeats the full
# 11-tab walk at early/middle/late offsets instead of once) and the extra
# end-of-capture checks it runs -- see that file's own header.
#
# Run via `make demo-rehearsal` (scripts/demo-rehearsal.sh), which either
# creates a throwaway Hetzner VM (the default, private to this one run) or
# -- when PGWT_BOX is set -- targets a PERSISTENT, shared box instead
# (owner rule 2026-09-28: persistent boxes first; required for a run that
# is meant to count toward docs/DEMO_REHEARSAL_CRITERIA.md's sequence).
# Not wired into tests/run_all.sh and never should be -- it is 30-45x
# longer than everything else run_all.sh does.
#
# No flock in THIS script (unlike tests/ui_live_smoke.sh) -- the lock now
# lives one level up, in scripts/demo-rehearsal-remote-run.sh (which wraps
# THIS whole script, `sudo`+build included, in
# /tmp/pgwt-box-check.lock -- the same lock box-check.sh uses), because
# that is the process that is actually detached and long-lived, and taking
# it there means an uncontended throwaway-VM run pays nothing extra while
# a PERSISTENT-box run correctly queues behind CI/other agents instead of
# colliding with them. remote-run.sh's `sudo ... tests/demo_rehearsal.sh`
# invocation (which is what actually starts everything below: tracer,
# bridge, pgwt-server, pgbench, the lock/sleep workload) is deliberately
# run with `200>&-` so none of THIS script's own children ever inherit
# that fd -- see the remote-run.sh comment for why an inherited fd on a
# SHARED persistent box is now a real hazard, not just a hypothetical one:
# cleanup() below already detects a leaked child and exits 1 but does NOT
# kill it, exactly like tests/ui_live_smoke.sh's own cleanup() documents.
#
# Usage: tests/demo_rehearsal.sh [--pid POSTMASTER_PID] [--pg-version N] [--tag TAG]
# Env: DURATION_MIN (default 35), PGWT_DEMO_PORT (default 8385)
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
source "$SCRIPT_DIR/testutil.sh"
source "$SCRIPT_DIR/live_daemon_lib.sh"

TRACER="$PROJECT_DIR/pg_wait_tracer"
SERVER="$PROJECT_DIR/pgwt-server"
BRIDGE="$PROJECT_DIR/web/pgwt"

PM_PID=""
PG_MAJOR=""
# --tag: the git tag this attempt ran against (docs/DEMO_REHEARSAL_
# CRITERIA.md "a counted attempt names the tag it ran against"), passed
# down from scripts/demo-rehearsal-remote-run.sh -- forwarded to
# demo_rehearsal.py's own --tag below via tag_cli_args(). Left empty
# (the default) means "let demo_rehearsal.py auto-detect via `git
# describe`", which cannot work on THIS copy anyway since
# scripts/demo-rehearsal.sh's rsync excludes .git -- so an uncounted or
# ad-hoc run (no --tag given) still honestly records tag: null, never a
# guessed value.
TAG=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --pid) PM_PID="$2"; shift 2 ;;
        --pg-version) PG_MAJOR="$2"; shift 2 ;;
        --tag) TAG="$2"; shift 2 ;;
        *) echo "Usage: $0 [--pid POSTMASTER_PID] [--pg-version N] [--tag TAG]"; exit 1 ;;
    esac
done

# Default 35 min schedules the three passes; DURATION_MIN=3 is the explicit
# mechanics self-test. Validate before PostgreSQL discovery, provisioning,
# sysctl changes or load.
# DURATION_MIN schedules the walk; teardown owns both workload lifetimes.
DURATION_MIN="${DURATION_MIN:-35}"
if ! PYTHONPATH="$SCRIPT_DIR${PYTHONPATH:+:$PYTHONPATH}" python3 - "$DURATION_MIN" <<'PY'
import sys
from demo_rehearsal_lib import rehearsal_plan
try:
    duration_s = float(sys.argv[1]) * 60
    ticks, passes = rehearsal_plan(duration_s)
except ValueError as exc:
    sys.exit(f"REFUSING rehearsal plan: {exc}")
print(f"demo_rehearsal: launch plan target={duration_s:g}s ticks_per_tab={ticks} "
      f"passes={len(passes)}; workloads stay live until teardown")
if duration_s == 180:
    print("demo_rehearsal: 3-minute mechanics self-test; reduced coverage cannot pass the baseline")
PY
then
    exit 1
fi

if [[ -z "$PM_PID" ]]; then
    if [[ -n "$PG_MAJOR" ]]; then
        PM_PID=$(find_postmaster --pg-version "$PG_MAJOR")
    else
        PM_PID=$(find_postmaster)
    fi
fi
if [[ -z "$PM_PID" ]]; then
    echo "ERROR: cannot find postmaster PID"
    exit 1
fi
echo "demo_rehearsal: postmaster PID $PM_PID"

derive_pgport "$PM_PID" "$PG_MAJOR" "demo_rehearsal"

for bin in "$TRACER" "$SERVER" "$BRIDGE"; do
    if [[ ! -x "$bin" ]]; then
        echo "ERROR: $bin not built (make / make pgwt-client)"
        exit 1
    fi
done

PORT="${PGWT_DEMO_PORT:-8385}"

# Retention (docs/DEMO_DELIVERY_QUEUE.md item 2): ON by default since
# issue #386. It used to default to 0 "because a 40-minute --mode full
# trace is large" -- and then two rehearsals in a row destroyed the exact
# artifacts needed to diagnose their own failures (the offcpu_cap failures
# at 10.04% and 11.48% could not be investigated because the trace was
# gone). A diagnostic that deletes its evidence is worse than no
# diagnostic, and a re-run costs 35-45 minutes of box time plus a burned
# rehearsal attempt, which is worth far more than the disk. RETAIN_TRACE=0
# is still accepted as an explicit opt-out for a cheap mechanics run.
# When "1", cleanup() below keeps TRACE_DIR (moved to
# tests/results/demo_rehearsal/trace/, which scripts/demo-rehearsal.sh
# already rsyncs back unmodified -- no new sync path needed) instead of
# deleting it, and demo_rehearsal.py additionally saves the raw aas/
# time_model responses its cross-tab checks queried. Exported so the
# python child (PGWT_RETAIN_TRACE) sees it regardless of whether the
# caller set it (then it is already in the environment) or left it to
# this script's own default.
RETAIN_TRACE="${RETAIN_TRACE:-1}"
export RETAIN_TRACE
export PGWT_RETAIN_TRACE="$RETAIN_TRACE"

# issue #384 diagnostic, OFF by default: DIAG_DATA=1 adds --diag-data to the
# python child, which records WHAT THE CHART WAS RENDERING at each compared
# frame (one PNG + one JSON per tick under <pass>/<tab>/diag/). It costs
# extra round trips per tick and changes no verdict, so a counted attempt
# leaves it off; it exists so a blink observed during a rehearsal can be
# traced to the data behind the frame instead of being re-guessed from a
# screenshot. tests/test_demo_rehearsal_diag.py pins the whole thread.
DIAG_DATA="${DIAG_DATA:-0}"
DIAG_CLI_ARGS=()
if [[ "$DIAG_DATA" == 1 ]]; then
    # An `[[ ... ]] && assign` one-liner here would be a non-zero final
    # status under `set -e`: the default (DIAG_DATA=0) path would abort the
    # whole rehearsal before it started.
    DIAG_CLI_ARGS=(--diag-data)
fi

# ── kernel run_delay accounting (issue #386) ─────────────────────────────
# /proc/<pid>/schedstat field 2 (run_delay: ns runnable but NOT on a CPU)
# is the ONLY way to measure the run-queue component of the Off-CPU*
# residual independently of our own instrumentation -- and the kernel only
# accumulates it when this sysctl is 1. Measured 2026-10-13: it is 0 by
# default on both gate boxes, so without this the counters sit at 0 forever
# and the decomposition would credit the whole residual to "unexplained"
# with no evidence. tests/schedstat_lib.py REFUSES in that state rather
# than reporting a 0 (so a run that cannot enable it is visibly
# unmeasured, never silently clean); this just makes the normal case
# measurable. Restored to its original value in cleanup() -- it is a global
# sysctl and leaving it flipped would change every later measurement taken
# on a shared box.
# Declared HERE (before cleanup() is defined and the EXIT trap armed)
# because cleanup() reads both under `set -u`; the actual flip happens
# right AFTER `trap cleanup EXIT` below, so the restore path is always
# armed before the value is ever changed.
SCHEDSTATS_PATH=/proc/sys/kernel/sched_schedstats
SCHEDSTATS_ORIG=""
TRACE_DIR=$(mktemp -d /tmp/pgwt_demo_XXXXXX)
DAEMON_LOG=$(mktemp /tmp/pgwt_demo_daemon_XXXXXX.log)
BRIDGE_LOG=$(mktemp /tmp/pgwt_demo_bridge_XXXXXX.log)
WORKLOAD_LOG=$(mktemp /tmp/pgwt_demo_workload_XXXXXX.log)
PGBENCH_LOG=$(mktemp /tmp/pgwt_demo_pgbench_XXXXXX.log)
SOCK="$TRACE_DIR/pgwt.sock"

TRACER_PID=""
BRIDGE_PID=""
WORKLOAD_PID=""
PGBENCH_PID=""
DEMO_RC=1

# stop_pid: bounded wait (signal, poll, KILL) -- live_daemon_lib.sh. The
# bridge gets SIGINT specifically (web/main.go only handles os.Interrupt).
cleanup() {
    # issue #386: put the global sysctl back exactly as it was, BEFORE
    # anything else can exit early below.
    if [[ -n "$SCHEDSTATS_ORIG" && "$SCHEDSTATS_ORIG" != "1" ]]; then
        if sysctl -q -w "kernel.sched_schedstats=$SCHEDSTATS_ORIG" 2>/dev/null; then
            echo "demo_rehearsal: restored kernel.sched_schedstats=$SCHEDSTATS_ORIG"
        else
            echo "demo_rehearsal: WARNING could not restore kernel.sched_schedstats=$SCHEDSTATS_ORIG -- it is still $(cat "$SCHEDSTATS_PATH" 2>/dev/null)" >&2
        fi
    fi

    stop_pid "$WORKLOAD_PID" 10
    stop_pid "$PGBENCH_PID" 10
    stop_pid "$BRIDGE_PID" 10 INT
    stop_pid "$TRACER_PID" 15

    echo "demo_rehearsal: daemon log tail:"
    tail -n 60 "$DAEMON_LOG" 2>/dev/null | sed 's/^/  /'
    echo "demo_rehearsal: bridge log tail:"
    tail -n 40 "$BRIDGE_LOG" 2>/dev/null | sed 's/^/  /'
    echo "demo_rehearsal: workload log tail:"
    tail -n 20 "$WORKLOAD_LOG" 2>/dev/null | sed 's/^/  /'
    tail -n 20 "$PGBENCH_LOG" 2>/dev/null | sed 's/^/  /'

    # Same leaked-process guard as tests/ui_live_smoke.sh (issue #93 review
    # item 4). On a throwaway VM a leftover process here still gets swept
    # away by that VM's own deletion right after this script exits -- but
    # on a PERSISTENT box (PGWT_BOX) nothing ever deletes it, so a leftover
    # process is no longer just an untidy-teardown signal, it is a
    # potentially permanent resident. Reported loudly (exit 1) either way;
    # this guard does not kill it, which is exactly why remote-run.sh's
    # `200>&-` matters -- the flock fd must not be among what survives.
    sleep 1
    local leftover=""
    leftover="$(pgrep -f "$TRACE_DIR" 2>/dev/null || true)"
    local pid
    for pid in "$PGBENCH_PID" "$WORKLOAD_PID" "$BRIDGE_PID" "$TRACER_PID"; do
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            leftover="$leftover"$'\n'"$pid"
        fi
    done
    leftover="$(echo "$leftover" | sed '/^$/d')"

    if [[ "$RETAIN_TRACE" == "1" ]]; then
        # tests/results/demo_rehearsal/ (never /tmp) so scripts/demo-
        # rehearsal.sh's EXISTING rsync of that one directory picks this up
        # too -- no separate sync path to add or keep in sync.
        retain_dir="$PROJECT_DIR/tests/results/demo_rehearsal/trace"
        rm -rf "$retain_dir"
        mkdir -p "$(dirname "$retain_dir")"
        if mv "$TRACE_DIR" "$retain_dir" 2>/dev/null; then
            echo "demo_rehearsal: RETAIN_TRACE=1 -- kept the trace dir at $retain_dir (syncs back with tests/results/demo_rehearsal/)"
        else
            echo "demo_rehearsal: RETAIN_TRACE=1 but mv $TRACE_DIR -> $retain_dir failed -- removing it instead of leaking it in /tmp" >&2
            rm -rf "$TRACE_DIR"
        fi
    else
        rm -rf "$TRACE_DIR"
    fi
    rm -f "$DAEMON_LOG" "$BRIDGE_LOG" "$WORKLOAD_LOG" "$PGBENCH_LOG"

    if [[ -n "$leftover" ]]; then
        echo "ERROR: processes from this run survived teardown:"
        echo "$leftover" | while read -r p; do
            [[ -n "$p" ]] && ps -o pid,ppid,cmd -p "$p" 2>/dev/null
        done
        exit 1
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# issue #386: flip the kernel's run_delay accounting ON now that the
# restore path (cleanup, above) is armed.
if [[ -r "$SCHEDSTATS_PATH" ]]; then
    SCHEDSTATS_ORIG="$(cat "$SCHEDSTATS_PATH" 2>/dev/null || true)"
    if [[ "$SCHEDSTATS_ORIG" == "1" ]]; then
        echo "demo_rehearsal: kernel.sched_schedstats already 1 -- run_delay is being accumulated"
    elif sysctl -q -w kernel.sched_schedstats=1 2>/dev/null; then
        echo "demo_rehearsal: kernel.sched_schedstats $SCHEDSTATS_ORIG -> 1 for this run (restored at teardown)"
    else
        echo "demo_rehearsal: WARNING could not set kernel.sched_schedstats=1 (was '$SCHEDSTATS_ORIG') -- the Off-CPU* decomposition will REFUSE every window instead of reporting an unmeasured 0" >&2
    fi
else
    echo "demo_rehearsal: WARNING $SCHEDSTATS_PATH is not readable (no CONFIG_SCHEDSTATS?) -- the Off-CPU* decomposition will REFUSE every window" >&2
fi


# criteria doc (docs/DEMO_REHEARSAL_CRITERIA.md) §7: "no test anywhere in
# the run exited 126 (not executable) or 127 (command not found)" -- that
# is a broken invocation, never a known product bug. The kill -0 checks
# below already catch ANY early death regardless of its exit code; this
# just reaps the already-dead PID (harmless -- `wait` on a dead PID returns
# immediately) and names 126/127 specifically so that failure mode is never
# mistaken for a product regression in the log a human reads afterward.
report_early_exit() {
    local label="$1" pid="$2"
    local rc=0
    wait "$pid" 2>/dev/null; rc=$?
    if [[ "$rc" -eq 126 || "$rc" -eq 127 ]]; then
        echo "demo_rehearsal: $label exited $rc -- BROKEN INVOCATION (not executable / command not found), not a product bug"
    else
        echo "demo_rehearsal: $label exited with code $rc"
    fi
}

# ── 1. Controlled load ───────────────────────────────────────────────────────
check_pgbench_provisioned "demo_rehearsal" "$PGBENCH_LOG"

echo "demo_rehearsal: starting pgbench (4 clients, until teardown, throttled)"
# --rate: same 25 tx/s reasoning as tests/ui_live_smoke.sh (an unthrottled
# --mode full capture over the WHOLE window grows the executions dataset
# enough to make the end-of-capture Waterfall-latency check meaningless --
# it would be measuring "how slow does an unrealistically large dataset
# get", not the realistic-load regression issue #101 describes).
# pgbench requires a finite duration or transaction count. INT_MAX per client
# at 25 total tx/s cannot finish within a rehearsal (~10.9 years); cleanup()
# stops this same process on success, failure, INT or TERM. No restart loop:
# an unexpected exit must still trip the walk's unchanged liveness guard.
pgbench -U postgres -d postgres -c 4 -t 2147483647 --rate=25 \
    >>"$PGBENCH_LOG" 2>&1 &
PGBENCH_PID=$!
sleep 1
if ! kill -0 "$PGBENCH_PID" 2>/dev/null; then
    echo "ERROR: pgbench exited immediately after starting:"
    report_early_exit "pgbench" "$PGBENCH_PID"
    tail -n 20 "$PGBENCH_LOG"
    exit 1
fi

echo "demo_rehearsal: starting looping lock/sleep workload"
python3 "$SCRIPT_DIR/live_loop_workload.py" --until-stopped >>"$WORKLOAD_LOG" 2>&1 &
WORKLOAD_PID=$!
sleep 2
if ! kill -0 "$WORKLOAD_PID" 2>/dev/null; then
    echo "ERROR: lock/sleep workload exited immediately after starting:"
    report_early_exit "lock/sleep workload" "$WORKLOAD_PID"
    tail -n 40 "$WORKLOAD_LOG"
    exit 1
fi

# ── 2. Daemon (--mode full -- see tests/ui_live_smoke.sh's DEVIATION note
# for why full, not tiered: plan/execute USDT probes, which Waterfall's
# executions query needs, are gated on --mode full only). --daemon: long-
# running, no fixed capture budget -- the walk below decides how long.
echo "demo_rehearsal: starting daemon (--mode full, trace dir $TRACE_DIR)"
"$TRACER" --daemon --pid "$PM_PID" -i 1 -T "$TRACE_DIR" \
    --mode full -v \
    >/dev/null 2>"$DAEMON_LOG" &
TRACER_PID=$!

for _ in $(seq 1 60); do
    [[ -S "$SOCK" ]] && break
    if ! kill -0 "$TRACER_PID" 2>/dev/null; then
        echo "ERROR: daemon exited during startup:"
        report_early_exit "daemon" "$TRACER_PID"
        tail -n 40 "$DAEMON_LOG"
        exit 1
    fi
    sleep 0.5
done
if [[ ! -S "$SOCK" ]]; then
    echo "ERROR: control socket never appeared at $SOCK"
    tail -n 40 "$DAEMON_LOG"
    exit 1
fi
echo "demo_rehearsal: control socket ready"

# ── 3. Go bridge ─────────────────────────────────────────────────────────────
echo "demo_rehearsal: starting Go bridge on :$PORT against root@localhost"
"$BRIDGE" --port "$PORT" --trace-dir "$TRACE_DIR" --server-path "$SERVER" \
    root@localhost >"$BRIDGE_LOG" 2>&1 &
BRIDGE_PID=$!

BASE_URL="http://localhost:$PORT/"
for _ in $(seq 1 60); do
    if url_ready "${BASE_URL}session"; then
        break
    fi
    if ! kill -0 "$BRIDGE_PID" 2>/dev/null; then
        echo "ERROR: bridge exited during startup:"
        report_early_exit "bridge" "$BRIDGE_PID"
        tail -n 40 "$BRIDGE_LOG"
        exit 1
    fi
    sleep 0.5
done
if ! url_ready "${BASE_URL}session"; then
    echo "ERROR: bridge never answered ${BASE_URL}session"
    tail -n 40 "$BRIDGE_LOG"
    exit 1
fi
echo "demo_rehearsal: bridge ready at $BASE_URL"

# The bridge serves web/static from INSIDE its own binary (web/main.go
# `//go:embed static`), so a web/pgwt built before a UI edit keeps serving the
# old assets and this whole rehearsal would pass against the wrong UI with no
# outward sign -- exactly the failure that cannot be detected afterwards from
# the summary. Refuse here, before the CONTROLLED walk, naming the asset and
# both hashes. "Before the walk", not "before any page load": web/main.go's
# startup calls openBrowser(), so on a desktop session a browser may already
# have fetched the page -- what the rehearsal GRADES is gated, a human's own
# tab is not.
# See assert_served_ui_matches_tree in live_daemon_lib.sh for the exit-code
# contract (1 = served bytes differ, 2 = could not look at all, 3 = python3
# missing; every one of them refuses).
if ! assert_served_ui_matches_tree "$BASE_URL" "$PROJECT_DIR/web/static" "demo_rehearsal"; then
    echo "ERROR: the bridge is not serving this tree's web/static -- rebuild with 'make pgwt-client' and rerun"
    exit 1
fi

# ── 4. The rehearsal: repeated walk + end-of-capture checks ─────────────────
# Daemon and bridge stay UP through this whole call (including the
# end-of-capture pgwt-server queries tests/demo_rehearsal.py issues
# directly against $TRACE_DIR) -- teardown only happens in cleanup() above,
# after this returns, same ordering as tests/ui_live_smoke.sh.
mapfile -t TAG_CLI_ARGS < <(tag_cli_args "$TAG")
python3 -u "$SCRIPT_DIR/demo_rehearsal.py" --url "$BASE_URL" \
    --trace-dir "$TRACE_DIR" --daemon-log "$DAEMON_LOG" \
    --duration-min "$DURATION_MIN" \
    --pgbench-pid "$PGBENCH_PID" --workload-pid "$WORKLOAD_PID" \
    --daemon-pid "$TRACER_PID" --postmaster-pid "$PM_PID" \
    "${DIAG_CLI_ARGS[@]+${DIAG_CLI_ARGS[@]}}" "${TAG_CLI_ARGS[@]}"
DEMO_RC=$?

exit "$DEMO_RC"
