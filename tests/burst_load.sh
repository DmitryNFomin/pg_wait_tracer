#!/bin/bash
# burst_load.sh -- the BURST tier of the demo workload: short, concurrent
# pgbench bursts that put a realistic OLTP shape on the wait profile.
#
# WHY IT EXISTS. tests/live_loop_workload.py drives single psql pipes, one
# statement at a time, with client-side sleeps between them. That shape can
# manufacture any named wait class, but it cannot manufacture CONTENTION:
# a wait class whose cost comes from many backends hitting the same lock,
# WAL buffer or buffer-mapping partition at the same instant needs many
# backends doing it at the same instant. pgbench with -c/-j and a --rate is
# the tool for that. Measured per-component on the gate box (see "Burst
# sizing" below), the WAL burst alone moved LWLock:WALWrite from ~0% to
# 25.5% of DB Time -- the commit path becoming the largest class, which is
# what a pgbench-shaped OLTP system on a small shared_buffers actually
# looks like.
#
# PROVENANCE. Recovered from /root/workload/bursts.sh on the gate box, where
# it lived untracked. Parameterised on the way in: no /root/workload paths,
# no hardcoded port 5418, no /usr/lib/postgresql/18/bin PATH injection.
# PGPORT is INHERITED from the caller, which derived it with
# live_daemon_lib.sh's derive_pgport -- this script deliberately does not
# re-implement that derivation (CLAUDE.md "Extend, do not fork"; a second
# copy of the "never trust an ambient PGPORT" logic is exactly how one walk
# ended up tracing PG13 while loading PG18).
#
# THE GUARDS, AND WHY THERE ARE FIVE OF THEM. On 2026-10-02 an
# `rsync --delete` removed this rig's four .sql files. pgbench then printed
# "could not open file" and exited non-zero on every invocation -- and
# nobody noticed for three hours, because the driver of the day piped
# pgbench's stderr through `grep` and never looked at its exit status. The
# bursts were no-ops that looked like they worked. Every guard below exists
# to make that specific class of failure -- a burst that runs but does
# nothing -- impossible to mistake for success:
#
#   1. the sentinel .not-an-rsync-target in the SQL directory (directory
#      identity: a --delete from an unrelated source, a partial copy, or a
#      hand-rolled PGWT_BURST_SQL_DIR);
#   2. every .sql file present, non-empty and readable;
#   3. pgbench's EXIT STATUS, read directly from $? -- its output is
#      captured to a file and copied to the log, never piped through
#      anything that could swallow a diagnostic or mask the status;
#   4. a per-invocation achieved-tps FLOOR (a burst that connected and
#      exited cleanly having done almost nothing still fails);
#   5. an independent end-to-end proof for the WAL burst: pg_stat_user_tables'
#      n_tup_ins for pgwt_burst must actually have GROWN. This is the one
#      guard that does not take pgbench's word for anything.
#
# Plus two preconditions checked once before the first cycle, because the
# "thing being checked is absent rather than wrong" case is the one a tps
# floor cannot see: an io_read burst against a MISSING or EMPTY
# _smoke_io_load returns instantly at a very high tps and sails past every
# floor above while producing no IO:DataFileRead at all. A gate that cannot
# see must refuse, so the row count is verified up front.
#
# Usage: tests/burst_load.sh DURATION_S
#   DURATION_S  wall-clock budget; the cycle loop stops at the deadline.
#               SIGTERM/SIGINT also stop it cleanly (the callers' cleanup()
#               uses live_daemon_lib.sh's stop_pid, i.e. TERM then KILL).
# Env:
#   PGPORT              REQUIRED. Set by the caller's derive_pgport.
#   PGWT_BURST_SQL_DIR  SQL directory (default: tests/burst_sql next to this
#                       script). Overridable only so the guard tests can
#                       point it at a deliberately broken directory.
#   PGWT_BURST_LOG      append the full pgbench output here (default: a
#                       mktemp file, path echoed at startup).
#   PGWT_BURST_CYCLES   run exactly N cycles then exit 0 (default: unbounded
#                       until DURATION_S/SIGTERM). The box-side one-cycle
#                       proof uses PGWT_BURST_CYCLES=1.
#
# This script only actually RUNS on Linux with a live PostgreSQL; on the Mac
# it is covered by `bash -n` plus tests/test_burst_load_guards.sh, which
# drives every refusal path above against stub pgbench/psql binaries.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SQL_DIR="${PGWT_BURST_SQL_DIR:-$SCRIPT_DIR/burst_sql}"
LOG="${PGWT_BURST_LOG:-$(mktemp /tmp/pgwt_burst_XXXXXX.log)}"
CYCLES="${PGWT_BURST_CYCLES:-0}"

DURATION_S="${1:-}"
if [[ ! "$DURATION_S" =~ ^[0-9]+$ ]] || [[ "$DURATION_S" -le 0 ]]; then
    echo "burst_load: ERROR: usage: $0 DURATION_S (got '${DURATION_S}')" >&2
    exit 2
fi

# die: refuse loudly. NOT `kill -TERM 0` (which is what the untracked
# original did): this script is started as a BACKGROUND JOB of
# tests/ui_live_smoke.sh and tests/demo_rehearsal.sh, and a background job
# started without job control shares its parent's process group -- so
# signalling group 0 from here would TERM the whole live walk, the daemon
# and the bridge along with it. Reap only our OWN children.
die() {
    echo "burst_load: ABORT: $*" | tee -a "$LOG" >&2
    pkill -P $$ 2>/dev/null
    exit 1
}

trap 'pkill -P $$ 2>/dev/null; exit 0' INT TERM

# Interruptible sleep. bash runs a trap only BETWEEN commands, so a plain
# `sleep 54` would delay this script's SIGTERM handling by up to 54 s -- long
# past the callers' stop_pid budget, which would then KILL it and leave the
# teardown to the leftover-process guard. `wait` is the one builtin a trap
# can interrupt, so every inter-burst pause goes through this.
isleep() { sleep "$1" & wait $! 2>/dev/null; }

# ── Preconditions ───────────────────────────────────────────────────────────

# PGPORT: refuse rather than guess. The callers export it from
# derive_pgport; a bare manual run must do the same (and the manual run must
# also be inside `flock /tmp/pgwt-box-check.lock`, like every other
# CPU-using step on a shared box).
# GUARD-BEGIN:pgport
if [[ -z "${PGPORT:-}" || ! "${PGPORT}" =~ ^[0-9]+$ ]]; then
    echo "burst_load: ERROR: PGPORT is unset or not numeric (got '${PGPORT:-}')." \
         "This script does not derive it -- source tests/live_daemon_lib.sh and" \
         "call derive_pgport, the way tests/ui_live_smoke.sh and" \
         "tests/demo_rehearsal.sh do, or export PGPORT explicitly." >&2
    exit 1
fi
# GUARD-END:pgport

# pgbench/psql on PATH: named explicitly, because the recovered original
# injected /usr/lib/postgresql/18/bin and a missing binary would otherwise
# surface as a bare 127 from inside the cycle loop.
# GUARD-BEGIN:tooling
for tool in pgbench psql; do
    command -v "$tool" >/dev/null 2>&1 || \
        die "$tool not found on PATH -- a burst cannot run (this is a broken invocation, not a product bug)"
done
# GUARD-END:tooling

# Guard 1: the sentinel.
# GUARD-BEGIN:sentinel
[[ -f "$SQL_DIR/.not-an-rsync-target" ]] || \
    die "sentinel $SQL_DIR/.not-an-rsync-target is missing -- $SQL_DIR is not an intact checkout of tests/burst_sql (see that file for the 2026-10-02 incident this detects)"
# GUARD-END:sentinel

# Guard 2: every script present, non-empty, readable.
BURST_SQL_FILES=(burst.sql adv300.sql io_read.sql io_write.sql)
# GUARD-BEGIN:sqlfiles
for f in "${BURST_SQL_FILES[@]}"; do
    [[ -s "$SQL_DIR/$f" && -r "$SQL_DIR/$f" ]] || \
        die "$SQL_DIR/$f missing, empty or unreadable"
done
# GUARD-END:sqlfiles

PGB=(pgbench -n -U postgres -d postgres -p "$PGPORT")
psql_q() { psql -U postgres -p "$PGPORT" -d postgres -Atc "$1" 2>>"$LOG"; }

echo "burst_load: PGPORT=$PGPORT sql_dir=$SQL_DIR log=$LOG duration=${DURATION_S}s cycles=${CYCLES:-unbounded}"
{
    echo "### burst_load start $(date -u +%F' '%H:%M:%S) UTC PGPORT=$PGPORT"
    # Checksums of what we are about to run, so a stale-but-present .sql
    # file (the one thing guards 1 and 2 cannot see) is at least
    # attributable after the fact.
    (cd "$SQL_DIR" && md5sum "${BURST_SQL_FILES[@]}" 2>/dev/null \
        || md5 "${BURST_SQL_FILES[@]}" 2>/dev/null)
} >>"$LOG"

# b_wal's target table: tiny, created here (unlike _smoke_io_load, which is
# 3M rows and belongs to provisioning). n_tup_ins below is cumulative since
# the last stats reset, so a before/after DELTA is correct whether or not
# the table already existed.
psql_q "CREATE TABLE IF NOT EXISTS pgwt_burst (ts timestamptz, pad text)" >/dev/null || \
    die "could not create table pgwt_burst on port $PGPORT"
burst_tbl=$(psql_q "SELECT count(*) FROM pg_class WHERE relname = 'pgwt_burst'")
[[ "$burst_tbl" == "1" ]] || \
    die "table pgwt_burst is not present on port $PGPORT after CREATE (got count=${burst_tbl:-<empty>}) -- b_wal would insert nowhere"

# io_read/io_write's target table: provisioned (tests/provision-runner.sh),
# never built here. Checked for SIZE, not mere existence: an EMPTY
# _smoke_io_load makes io_read.sql return instantly at a very high tps,
# which passes guards 3 and 4 while producing no IO:DataFileRead at all.
IO_LOAD_MIN_ROWS="${PGWT_BURST_IO_MIN_ROWS:-3000000}"
io_rows=$(psql_q "SELECT count(*) FROM _smoke_io_load")
# GUARD-BEGIN:iorows
if [[ ! "$io_rows" =~ ^[0-9]+$ ]] || [[ "$io_rows" -lt "$IO_LOAD_MIN_ROWS" ]]; then
    die "_smoke_io_load has '${io_rows:-<unreadable>}' rows on port $PGPORT, want >= $IO_LOAD_MIN_ROWS -- tests/provision-runner.sh should have built it. A missing or short table makes the io_read/io_write bursts fast no-ops that pass every tps floor, which is the exact failure mode these guards exist to refuse."
fi
# GUARD-END:iorows
echo "burst_load: _smoke_io_load has $io_rows rows (>= $IO_LOAD_MIN_ROWS) -- IO bursts will miss shared_buffers" | tee -a "$LOG"

# ── Burst sizing ────────────────────────────────────────────────────────────
# From a per-component probe on the gate box (60 s each, alone, tracer
# attached), NOT from guesswork -- two 25-minute captures were spent
# guessing before the probe existed:
#   base pgbench c4 r25      2,116 events/s      loop workload      819
#   wal c6 rate1200         17,817               wal c6 rate400   5,939
#   advisory lock c4 gs300 104,516               same at gs30   108,817  (!)
#   advisory lock c2 gs300  55,569               io read c6 r300  4,096
#   io write c3 r150        17,203               idle (4 clusters)   68
# The advisory-lock burst dominates and is INSENSITIVE to statement size:
# nothing rate-limits it, so a cheaper statement just runs more often. Its
# cost is therefore controlled by CLIENTS and DURATION, which is why b_lm
# runs for 1 s, not the 6-8 s the others get.
#
# The tps FLOORS are set one to two orders of magnitude below the measured
# figures above. That is deliberate: a floor's job here is to detect a burst
# that did essentially NOTHING (the 2026-10-02 no-op), not to grade the box.
# A floor near the measured value would go red on a loaded shared box and
# would be hardening a test against runner noise, which CLAUDE.md forbids.

# run TAG TPS_FLOOR ARGS...
# Runs one pgbench invocation and checks three independent things: its exit
# status, its achieved tps against TPS_FLOOR, and that it processed a
# non-zero number of transactions. pgbench's whole output (stdout+stderr) is
# captured to a file and copied into the log -- never piped through grep.
run() {
    local tag="$1" floor="$2"
    shift 2
    local o
    o=$(mktemp "/tmp/pgwt_burst_${tag}_XXXXXX.out")
    echo "### $tag $(date -u +%H:%M:%S)" >>"$LOG"
    PGAPPNAME="$tag" "${PGB[@]}" "$@" >"$o" 2>&1
    local rc=$?
    cat "$o" >>"$LOG"
    local out
    out=$(cat "$o")
    rm -f "$o"
# GUARD-BEGIN:rc
    if [[ $rc -ne 0 ]]; then
        if [[ $rc -eq 126 || $rc -eq 127 ]]; then
            die "$tag: pgbench exited $rc -- BROKEN INVOCATION (not executable / command not found), not a product bug"
        fi
        die "$tag exited $rc -- pgbench output follows:"$'\n'"$out"
    fi
# GUARD-END:rc
    local tps n
    tps=$(awk '/^tps = /{print int($3); exit}' <<<"$out")
    n=$(awk -F'[:/ ]+' '/^number of transactions actually processed/{print $6; exit}' <<<"$out")
    # An unparseable tps/count is a REFUSAL, not a pass: a pgbench whose
    # summary format we cannot read is a gate that cannot see.
# GUARD-BEGIN:parse
    [[ "$tps" =~ ^[0-9]+$ ]] || \
        die "$tag: could not parse achieved tps out of pgbench's summary (got '${tps}') -- refusing to treat an unreadable result as a pass. Output:"$'\n'"$out"
    [[ "$n" =~ ^[0-9]+$ ]] || \
        die "$tag: could not parse the processed-transaction count (got '${n}') -- refusing to treat an unreadable result as a pass. Output:"$'\n'"$out"
# GUARD-END:parse
# GUARD-BEGIN:floor
    [[ "$tps" -ge "$floor" ]] || \
        die "$tag achieved tps=$tps, below floor $floor -- the burst connected but did essentially nothing"
# GUARD-END:floor
# GUARD-BEGIN:ntx
    [[ "$n" -gt 0 ]] || die "$tag processed $n transactions"
# GUARD-END:ntx
    echo "burst_load: $tag ok (tps=$tps >= $floor, $n transactions)" >>"$LOG"
}

# Independent end-to-end proof that the WAL burst's rows reached the
# database -- not just that pgbench reported success.
ins() { psql_q "SELECT n_tup_ins FROM pg_stat_user_tables WHERE relname = 'pgwt_burst'"; }

deadline=$(( $(date +%s) + DURATION_S ))
cycle=0
while [[ $(date +%s) -lt $deadline ]]; do
    before=$(ins)
    run b_wal 100 -c 6 -j 2 -T 8 --rate=600 -f "$SQL_DIR/burst.sql"
    after=$(ins)
# GUARD-BEGIN:walparse
    if [[ ! "$before" =~ ^[0-9]+$ || ! "$after" =~ ^[0-9]+$ ]]; then
        die "could not read n_tup_ins for pgwt_burst (before='${before}' after='${after}') -- refusing to approve a WAL burst whose end-to-end proof could not be evaluated"
    fi
# GUARD-END:walparse
    d=$(( after - before ))
    echo "### b_wal rows inserted: $d" >>"$LOG"
# GUARD-BEGIN:walrows
    [[ "$d" -gt 0 ]] || \
        die "b_wal inserted $d rows into pgwt_burst -- pgbench reported success but nothing reached the table (the 2026-10-02 no-op signature)"
# GUARD-END:walrows
    isleep 32
    [[ $(date +%s) -lt $deadline ]] || break

    run b_lm 500 -c 4 -j 2 -T 1 -f "$SQL_DIR/adv300.sql"
    isleep 37
    [[ $(date +%s) -lt $deadline ]] || break

    # Read and write bursts overlap on purpose: concurrent readers and
    # writers on the same above-shared_buffers table is what makes the
    # buffer-mapping and eviction paths contend.
    run b_ior 50 -c 6 -j 2 -T 6 --rate=300 -f "$SQL_DIR/io_read.sql" &
    ior_pid=$!
    run b_iow 10 -c 3 -j 1 -T 6 --rate=50 -f "$SQL_DIR/io_write.sql"
    # `run` dies (exit 1) inside the subshell on failure, so the background
    # burst's status must be collected, not discarded.
    if ! wait "$ior_pid"; then
        die "b_ior failed (see $LOG)"
    fi

    cycle=$((cycle + 1))
    if [[ $cycle -eq 1 ]]; then
        echo "burst_load: first cycle OK -- all four bursts did measurable work" | tee -a "$LOG"
    fi
    if [[ "$CYCLES" -gt 0 && $cycle -ge "$CYCLES" ]]; then
        echo "burst_load: completed $cycle cycle(s) as requested (PGWT_BURST_CYCLES=$CYCLES)" | tee -a "$LOG"
        exit 0
    fi
    isleep 54
done

echo "burst_load: duration budget reached after $cycle cycle(s)" | tee -a "$LOG"
# GUARD-BEGIN:nocycle
if [[ $cycle -eq 0 ]]; then
    die "the duration budget (${DURATION_S}s) expired before a single cycle completed -- one cycle needs ~2.5 min, so this run proved nothing"
fi
# GUARD-END:nocycle
exit 0
