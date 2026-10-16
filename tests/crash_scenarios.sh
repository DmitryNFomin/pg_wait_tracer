#!/usr/bin/env bash
# crash_scenarios.sh — #424: kill a REAL daemon against a REAL PostgreSQL and
# check that the capture it leaves behind tells the truth about how it ended.
#
# The C unit tests (tests/test_trace_closure.c, tests/test_open_ckpt.c) pin the
# writer and recovery logic deterministically. This script answers the one
# question they cannot: does a daemon that is SIGKILLed while backends sit in
# real waits leave a file that a reader can recognise as an unclean capture?
#
# Scenarios run one at a time, each in its OWN trace directory. The directory
# isolation is what keeps one scenario's leftovers out of another's recovery;
# the serialisation is because the BPF watchpoint on a single postmaster is a
# shared resource, so six daemons attached at once would contend rather than
# test anything. Scenarios against DIFFERENT postmasters are independent and
# can be sharded across machines.
#
# Scenarios:
#   open_wait      kill while a backend is inside a long wait (pg_sleep)
#   open_cpu       kill while a backend is burning CPU inside a command
#   publication    kill while transitions are flowing at full rate (pgbench)
#   early          kill before any block was committed (nothing to archive)
#   repeated       kill, restart, kill again, restart again
#   graceful       let the daemon exit NORMALLY — the control case
#   ckpt_survives  an open-interval CHECKPOINT beside a live capture must
#                  survive the kill and be carried to the archive, with its
#                  records intact — closure labels alone cannot see this,
#                  which is how two handover bugs survived a "15/15 PASS"
#   ckpt_clean     the other side of that rule: a graceful shutdown DROPS the
#                  checkpoint, because the real closing records were written
#   ckpt_window    a daemon killed INSIDE recovery's handover (named crash
#                  point) must leave a state the next start completes
#
# `graceful` and `ckpt_clean` are not decoration: without them, a script that
# printed "unclean" for everything, or one that kept every checkpoint
# unconditionally, would pass. The run FAILS if graceful does not come back
# `clean` and if ckpt_clean does not come back `none`, which is only possible
# if the product distinguishes the cases.
#
# The checkpoint producer is NOT wired into the daemon yet, so those three
# scenarios use tests/open_ckpt_stub to place a validly-identified checkpoint
# beside the live capture. Everything after that — kill, recovery, handover,
# retention — is the real product.
#
# Usage: sudo tests/crash_scenarios.sh --pid <postmaster-pid> --psql "<psql cmd>"
#                                      [--out DIR]
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PM_PID=""
PSQL=""
OUT="$PROJECT_DIR/tests/results/issue424-durability"
PGBENCH="pgbench"
# The tree this run is evidence FOR. The caller passes it because a box that
# was rsynced without .git cannot answer it, and a log that cannot name its
# commit is not evidence of anything.
COMMIT=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --pid) PM_PID="$2"; shift 2 ;;
        --psql) PSQL="$2"; shift 2 ;;
        --pgbench) PGBENCH="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --commit) COMMIT="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

# ── Refuse, never skip ────────────────────────────────────────
# A missing prerequisite means this check could not look at anything. That is
# not a pass. Every one of these exits 2 with REFUSED on the first line.
refuse() { echo "REFUSED: $*" >&2; exit 2; }

[[ -n "$PM_PID" ]] || refuse "--pid <postmaster-pid> is required"
[[ -n "$PSQL" ]]   || refuse "--psql '<psql command>' is required"
[[ "$(uname -s)" == "Linux" ]] || refuse "eBPF capture is Linux-only"
[[ "$(id -u)" == "0" ]] || refuse "must run as root (BPF + trace dirs)"
[[ -x "$PROJECT_DIR/pg_wait_tracer" ]] || refuse "pg_wait_tracer is not built"
PROBE="$PROJECT_DIR/tests/trace_closure_probe"
[[ -x "$PROBE" ]] || refuse "tests/trace_closure_probe is not built (make -C tests trace_closure_probe)"
STUB="$PROJECT_DIR/tests/open_ckpt_stub"
[[ -x "$STUB" ]] || refuse "tests/open_ckpt_stub is not built (make -C tests open_ckpt_stub) — without it this script can only check closure labels, which is how two handover bugs survived a 15/15 run"
kill -0 "$PM_PID" 2>/dev/null || refuse "no process $PM_PID (postmaster)"
$PSQL -tAc "SELECT 1" >/dev/null 2>&1 || refuse "psql command does not work: $PSQL"

mkdir -p "$OUT"
LOG="$OUT/crash_scenarios.log"
: > "$LOG"
failures=0
summary=""

note() { echo "$*" | tee -a "$LOG"; }

# Everything the probe says about one file, as one line.
probe() { "$PROBE" "$1" 2>&1; }
field() { probe "$1" | sed -n "s/.* $2=\([^ ]*\).*/\1/p"; }

# Start a daemon in the background on its own trace dir and set DPID.
#
# NOT `DPID=$(start_daemon ...)`: a command substitution runs in a subshell, so
# the daemon would be that subshell's child and `wait $DPID` would return
# immediately with "not a child of this shell". The first version of this
# script did exactly that, and every "recovery produced no archive" failure was
# the script listing the directory while the recovering daemon was still
# starting up — a harness bug that looked like a product bug.
DPID=""
start_daemon() {
    local dir=$1 secs=$2
    mkdir -p "$dir"
    "$PROJECT_DIR/pg_wait_tracer" --mode tiered --pid "$PM_PID" \
        --trace-dir "$dir" --duration "$secs" --quiet \
        >>"$LOG" 2>&1 &
    DPID=$!
}

# Run a short daemon in the FOREGROUND purely so its startup recovery runs,
# and do not return until it has exited. Nothing may be asserted about an
# archive while the process that creates it is still alive.
recover_once() {
    local dir=$1
    "$PROJECT_DIR/pg_wait_tracer" --mode tiered --pid "$PM_PID" \
        --trace-dir "$dir" --duration 4 --quiet >>"$LOG" 2>&1
    local rc=$?
    if [[ $rc -ne 0 ]]; then
        note "  FAIL: the recovery daemon exited $rc — recovery did not run"
        failures=$((failures + 1))
        return 1
    fi
    return 0
}

# Wait until the daemon has committed at least one block (the meta watermark
# exists and is >= 1), or give up after `secs`. Returns 1 on give-up — the
# caller FAILS rather than killing a daemon that never captured anything,
# because "nothing was open" and "nothing was captured" must not be confused.
wait_for_block() {
    local dir=$1 secs=$2 i=0
    while [[ $i -lt $((secs * 10)) ]]; do
        if [[ -f "$dir/current.trace.meta" ]]; then
            local n
            n=$(cat "$dir/current.trace.meta" 2>/dev/null | tr -dc '0-9')
            [[ -n "$n" && "$n" -ge 1 ]] && return 0
        fi
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}

check() {
    local what=$1 expect=$2 got=$3
    if [[ "$expect" == "$got" ]]; then
        note "  PASS: $what ($got)"
    else
        note "  FAIL: $what — expected '$expect', got '$got'"
        failures=$((failures + 1))
    fi
}

# Background load so the capture has transitions to write in every scenario.
start_load() {
    local secs=$1
    ( $PSQL -c "SELECT pg_sleep($secs)" >/dev/null 2>&1 ) &
    echo $!
}

scenario_kill() {
    # scenario_kill <name> <workload-sql> — capture, run the workload, kill the
    # daemon mid-flight, restart it so recovery runs, then assert.
    local name=$1 sql=$2
    local dir="$OUT/$name"
    rm -rf "$dir"; mkdir -p "$dir"
    note ""
    note "[$name] capture → workload → SIGKILL → restart"

    start_daemon "$dir" 120
    local dpid=$DPID
    sleep 2
    ( eval "$PSQL -c \"$sql\"" >/dev/null 2>&1 ) &
    local wpid=$!

    if ! wait_for_block "$dir" 25; then
        note "  FAIL: [$name] the daemon committed no block in 25 s — this "
        note "        scenario cannot say anything about open intervals"
        failures=$((failures + 1))
        kill -9 "$dpid" "$wpid" 2>/dev/null
        wait "$dpid" 2>/dev/null
        return
    fi

    kill -9 "$dpid" 2>/dev/null
    wait "$dpid" 2>/dev/null
    kill -9 "$wpid" 2>/dev/null; wait "$wpid" 2>/dev/null

    note "  leftover: $(probe "$dir/current.trace")"
    check "[$name] the killed daemon's live file is unfinalized" \
          "in_progress" "$(field "$dir/current.trace" closure)"

    # Restart: recovery archives and labels it.
    recover_once "$dir" || return

    local arch
    arch=$(ls -1 "$dir"/*.trace.lz4 2>/dev/null | head -1)
    if [[ -z "$arch" ]]; then
        note "  FAIL: [$name] recovery produced no archive"
        failures=$((failures + 1))
        return
    fi
    note "  archive: $(probe "$arch")"
    check "[$name] the recovered archive is labelled unclean" \
          "unclean" "$(field "$arch" closure)"
    check "[$name] a reader is told open intervals may be missing" \
          "1" "$(field "$arch" may_lose_open_intervals)"
    summary="$summary$name=$(field "$arch" closure) "
}

scenario_early() {
    local dir="$OUT/early"
    rm -rf "$dir"; mkdir -p "$dir"
    note ""
    note "[early] kill before any block was committed"
    start_daemon "$dir" 120
    local dpid=$DPID
    sleep 0.7
    kill -9 "$dpid" 2>/dev/null; wait "$dpid" 2>/dev/null

    # Whatever is there, the one thing that must NOT happen is a clean label.
    local st="absent"
    [[ -f "$dir/current.trace" ]] && st=$(field "$dir/current.trace" closure)
    note "  leftover: ${st}"
    if [[ "$st" == "clean" ]]; then
        note "  FAIL: [early] a killed daemon's file must never read clean"
        failures=$((failures + 1))
    else
        note "  PASS: [early] not clean ($st)"
    fi
    recover_once "$dir"
    for a in "$dir"/*.trace.lz4; do
        [[ -e "$a" ]] || continue
        check "[early] no archive from this scenario claims clean" \
              "0" "$([[ "$(field "$a" closure)" == "clean" ]] && echo 1 || echo 0)"
    done
    summary="${summary}early=$st "
}

scenario_repeated() {
    local dir="$OUT/repeated"
    rm -rf "$dir"; mkdir -p "$dir"
    note ""
    note "[repeated] kill, restart, kill again, restart again"
    for round in 1 2; do
        start_daemon "$dir" 120
        local dpid=$DPID
        sleep 2
        ( $PSQL -c "SELECT pg_sleep(20)" >/dev/null 2>&1 ) &
        local wpid=$!
        if ! wait_for_block "$dir" 25; then
            note "  FAIL: [repeated] round $round committed no block"
            failures=$((failures + 1))
        fi
        kill -9 "$dpid" 2>/dev/null; wait "$dpid" 2>/dev/null
        kill -9 "$wpid" 2>/dev/null; wait "$wpid" 2>/dev/null
        recover_once "$dir"
    done
    # Three files end up here, and the labels must tell them apart: the two
    # SIGKILLed captures, plus the recovery daemon of round 1, which exited
    # normally and is therefore genuinely clean. "all unclean" would be the
    # wrong assertion — it would also pass if the labels were constant.
    local n=0 unclean=0 clean=0
    for a in "$dir"/*.trace.lz4; do
        [[ -e "$a" ]] || continue
        n=$((n + 1))
        case "$(field "$a" closure)" in
            unclean) unclean=$((unclean + 1)) ;;
            clean)   clean=$((clean + 1)) ;;
        esac
        note "  archive: $(probe "$a")"
    done
    check "[repeated] exactly the two crashed captures are labelled unclean" \
          "2" "$unclean"
    check "[repeated] and every other archive here is a clean exit" \
          "$((n - 2))" "$clean"
    [[ "$n" -ge 2 ]] || { note "  FAIL: [repeated] expected >=2 archives, got $n";
                          failures=$((failures + 1)); }
    summary="${summary}repeated=${unclean}unclean/${clean}clean "
}

scenario_graceful() {
    local dir="$OUT/graceful"
    rm -rf "$dir"; mkdir -p "$dir"
    note ""
    note "[graceful] the CONTROL case: a normal exit must read clean"
    start_daemon "$dir" 120
    local dpid=$DPID
    sleep 2
    ( $PSQL -c "SELECT pg_sleep(8)" >/dev/null 2>&1 ) &
    local wpid=$!
    if ! wait_for_block "$dir" 25; then
        note "  FAIL: [graceful] no block committed"
        failures=$((failures + 1))
    fi
    kill -TERM "$dpid" 2>/dev/null
    wait "$dpid" 2>/dev/null
    kill -9 "$wpid" 2>/dev/null; wait "$wpid" 2>/dev/null

    note "  closed file: $(probe "$dir/current.trace")"
    check "[graceful] a normal shutdown reads clean" \
          "clean" "$(field "$dir/current.trace" closure)"
    check "[graceful] and no warning is raised for it" \
          "0" "$(field "$dir/current.trace" may_lose_open_intervals)"

    recover_once "$dir"
    local arch; arch=$(ls -1 "$dir"/*.trace.lz4 2>/dev/null | head -1)
    if [[ -n "$arch" ]]; then
        note "  archive: $(probe "$arch")"
        check "[graceful] the clean label survives being archived by recovery" \
              "clean" "$(field "$arch" closure)"
    else
        note "  FAIL: [graceful] recovery produced no archive"
        failures=$((failures + 1))
    fi
    summary="${summary}graceful=$(field "$dir/current.trace" closure) "
}

scenario_checkpoint_survives_kill() {
    # The gate the closure-label scenarios cannot be: does the open-interval
    # CHECKPOINT survive a kill, with its records intact?
    #
    # The daemon has no checkpoint producer yet (pgwt_writer_checkpoint_open
    # has no caller in src/), so the stub writes one beside the live capture,
    # with the identity read from that capture's own header. Everything after
    # that — the kill, the recovery, the handover — is the real product.
    local dir="$OUT/ckpt_survives"
    rm -rf "$dir"; mkdir -p "$dir"
    note ""
    note "[ckpt_survives] checkpoint beside a live capture → SIGKILL → restart"

    start_daemon "$dir" 120
    local dpid=$DPID
    sleep 2
    ( $PSQL -c "SELECT pg_sleep(20)" >/dev/null 2>&1 ) &
    local wpid=$!
    if ! wait_for_block "$dir" 25; then
        note "  FAIL: [ckpt_survives] no block committed"
        failures=$((failures + 1))
        kill -9 "$dpid" "$wpid" 2>/dev/null; wait "$dpid" 2>/dev/null
        return
    fi

    local stub_out
    stub_out=$("$STUB" "$dir" 3 91000 2000000 2>&1)
    local stub_rc=$?
    note "  stub: $stub_out"
    if [[ $stub_rc -ne 0 ]]; then
        note "  FAIL: [ckpt_survives] could not write a checkpoint ($stub_rc)"
        failures=$((failures + 1))
        kill -9 "$dpid" "$wpid" 2>/dev/null; wait "$dpid" 2>/dev/null
        return
    fi
    # 2 + 4 + 6 ms, from the stub's own arithmetic.
    local want_ns=12000000

    check "[ckpt_survives] the live capture reports its checkpoint" \
          "seq=1,intervals=3,open_ns=$want_ns" \
          "$(field "$dir/current.trace" checkpoint)"

    kill -9 "$dpid" 2>/dev/null; wait "$dpid" 2>/dev/null
    kill -9 "$wpid" 2>/dev/null; wait "$wpid" 2>/dev/null
    check "[ckpt_survives] it is still there after the kill" \
          "seq=1,intervals=3,open_ns=$want_ns" \
          "$(field "$dir/current.trace" checkpoint)"

    recover_once "$dir" || return
    local arch
    arch=$(ls -1 "$dir"/*.trace.lz4 2>/dev/null | head -1)
    if [[ -z "$arch" ]]; then
        note "  FAIL: [ckpt_survives] recovery produced no archive"
        failures=$((failures + 1)); return
    fi
    note "  archive: $(probe "$arch")"
    check "[ckpt_survives] recovery carried it to the archive, intact" \
          "seq=1,intervals=3,open_ns=$want_ns" "$(field "$arch" checkpoint)"
    check "[ckpt_survives] and the archive is still labelled unclean" \
          "unclean" "$(field "$arch" closure)"
    summary="${summary}ckpt_survives=$(field "$arch" checkpoint) "
}

scenario_checkpoint_dropped_on_clean_close() {
    # The other side of the same rule, so "carried" cannot be satisfied by
    # keeping it unconditionally: a clean shutdown flushes the open intervals
    # as REAL records, and the checkpoint must then be gone.
    local dir="$OUT/ckpt_clean"
    rm -rf "$dir"; mkdir -p "$dir"
    note ""
    note "[ckpt_clean] checkpoint + graceful shutdown → it must be dropped"

    start_daemon "$dir" 120
    local dpid=$DPID
    sleep 2
    ( $PSQL -c "SELECT pg_sleep(12)" >/dev/null 2>&1 ) &
    local wpid=$!
    if ! wait_for_block "$dir" 25; then
        note "  FAIL: [ckpt_clean] no block committed"
        failures=$((failures + 1))
        kill -9 "$dpid" "$wpid" 2>/dev/null; wait "$dpid" 2>/dev/null
        return
    fi
    "$STUB" "$dir" 2 92000 3000000 >/dev/null 2>&1 ||
        { note "  FAIL: [ckpt_clean] stub failed"; failures=$((failures + 1)); }
    check "[ckpt_clean] the checkpoint is there before the shutdown" \
          "seq=1,intervals=2,open_ns=9000000" \
          "$(field "$dir/current.trace" checkpoint)"

    kill -TERM "$dpid" 2>/dev/null; wait "$dpid" 2>/dev/null
    kill -9 "$wpid" 2>/dev/null; wait "$wpid" 2>/dev/null

    check "[ckpt_clean] a clean close drops it" \
          "none" "$(field "$dir/current.trace" checkpoint)"
    check "[ckpt_clean] and the file is clean" \
          "clean" "$(field "$dir/current.trace" closure)"
    summary="${summary}ckpt_clean=$(field "$dir/current.trace" checkpoint) "
}

scenario_kill_inside_recovery_handover() {
    # The handover window, with the REAL daemon: a recovery that dies between
    # moving the checkpoint and moving the trace must leave a state the next
    # start completes, not one it cleans up.
    local dir="$OUT/ckpt_window"
    rm -rf "$dir"; mkdir -p "$dir"
    note ""
    note "[ckpt_window] daemon killed INSIDE recovery's handover"

    start_daemon "$dir" 120
    local dpid=$DPID
    sleep 2
    ( $PSQL -c "SELECT pg_sleep(20)" >/dev/null 2>&1 ) &
    local wpid=$!
    if ! wait_for_block "$dir" 25; then
        note "  FAIL: [ckpt_window] no block committed"
        failures=$((failures + 1))
        kill -9 "$dpid" "$wpid" 2>/dev/null; wait "$dpid" 2>/dev/null
        return
    fi
    "$STUB" "$dir" 2 93000 5000000 >/dev/null 2>&1 ||
        { note "  FAIL: [ckpt_window] stub failed"; failures=$((failures + 1)); }
    kill -9 "$dpid" 2>/dev/null; wait "$dpid" 2>/dev/null
    kill -9 "$wpid" 2>/dev/null; wait "$wpid" 2>/dev/null

    # A recovery that dies in the window. PGWT_CRASH_AT is inert without this
    # variable, so only this one process is affected.
    PGWT_CRASH_AT=recover_after_ckpt "$PROJECT_DIR/pg_wait_tracer" \
        --mode tiered --pid "$PM_PID" --trace-dir "$dir" --duration 4 --quiet \
        >>"$LOG" 2>&1
    local rc=$?
    note "  recovering daemon exit=$rc (137 = SIGKILL at the crash point)"
    if [[ $rc -ne 137 ]]; then
        note "  FAIL: [ckpt_window] the crash point did not fire (exit $rc) — "
        note "        this scenario proves nothing unless it does"
        failures=$((failures + 1))
        return
    fi

    local stranded
    stranded=$(ls -1 "$dir"/*.trace.lz4.open 2>/dev/null | head -1)
    check "[ckpt_window] the checkpoint moved first and waits for its trace" \
          "yes" "$([[ -n "$stranded" ]] && echo yes || echo no)"
    check "[ckpt_window] the trace is not archived yet" \
          "yes" "$([[ -f "$dir/current.trace" ]] && echo yes || echo no)"

    recover_once "$dir" || return
    local arch
    arch=$(ls -1 "$dir"/*.trace.lz4 2>/dev/null | head -1)
    if [[ -z "$arch" ]]; then
        note "  FAIL: [ckpt_window] no archive after the second start"
        failures=$((failures + 1)); return
    fi
    note "  archive: $(probe "$arch")"
    check "[ckpt_window] the interrupted handover completed, evidence intact" \
          "seq=1,intervals=2,open_ns=15000000" "$(field "$arch" checkpoint)"
    summary="${summary}ckpt_window=$(field "$arch" checkpoint) "
}

note "#424 crash scenarios — daemon=$PROJECT_DIR/pg_wait_tracer pm_pid=$PM_PID"
if [[ -z "$COMMIT" ]]; then
    COMMIT=$(cd "$PROJECT_DIR" && git rev-parse HEAD 2>/dev/null)
    [[ -n "$COMMIT" ]] || refuse "no --commit given and no git here: this run \
could not be attributed to a tree, and an unattributable log is not evidence"
fi
note "commit=$COMMIT"
note "kernel=$(uname -r) host=$(hostname)"

scenario_kill open_wait  "SELECT pg_sleep(25)"
scenario_kill open_cpu   "SELECT count(*) FROM generate_series(1, 400000000)"
scenario_kill publication "SELECT count(*) FROM generate_series(1, 50000000)"
scenario_early
scenario_repeated
scenario_graceful
scenario_checkpoint_survives_kill
scenario_checkpoint_dropped_on_clean_close
scenario_kill_inside_recovery_handover

note ""
note "summary: $summary"
if [[ "$failures" -eq 0 ]]; then
    note "crash_scenarios: ALL PASS"
    exit 0
fi
note "crash_scenarios: $failures FAILED"
exit 1
