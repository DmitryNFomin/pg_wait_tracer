#!/bin/bash
# test_burst_load_guards.sh -- the bypass suite for tests/burst_load.sh's
# twelve guards. Runs on the Mac: no PostgreSQL, no Linux, no root. Stub
# `pgbench`, `psql`, `sleep` and `date` binaries are put first on PATH, so
# every refusal path can be driven deliberately and the clock is PINNED
# (the stub `sleep` advances a fake clock file that the stub `date +%s`
# reads, so nothing in this suite depends on wall time or on how fast the
# machine is).
#
# WHY THIS FILE EXISTS, AND WHY IT HAS TWO PHASES.
#
# burst_load.sh's whole reason for being careful is the 2026-10-02 incident:
# its .sql files were deleted, every burst became a no-op, and three hours
# of captures were graded as good because the driver filtered pgbench's
# stderr through `grep` and never read its exit status. That is a FALSE
# NEGATIVE -- a gate that approved while the thing it was watching was
# completely broken. Showing that a guard CAN go red proves the easy half;
# the half that actually failed in 2026-10-02 is "the guard was never
# reached, or something else approved anyway".
#
# So:
#   Phase A (true positives). Each guard's own broken input, asserted to
#       produce a non-zero exit AND that guard's own signature on stderr.
#       Plus the happy path, asserted to exit 0 -- a suite where everything
#       refuses would "pass" while the rig could never run at all.
#   Phase B (reachability / non-redundancy). For each guard, burst_load.sh
#       is copied with THAT guard's code block deleted (the
#       `# GUARD-BEGIN:<name>` / `# GUARD-END:<name>` markers in the script
#       delimit exactly the lines that implement it), and the Phase A input
#       is re-run against the mutant. The mutant must NOT emit that guard's
#       signature. If it still does, the guard as written is not what
#       produces the refusal -- the check is redundant, or the marked block
#       is the wrong code -- and the Phase A pass was not evidence that this
#       guard works. The mutant's exit status is reported too: 0 means the
#       mutant positively APPROVED the broken input, which is the 2026-10-02
#       failure reproduced exactly.
#
# Usage: bash tests/test_burst_load_guards.sh
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BURST="$SCRIPT_DIR/burst_load.sh"
REAL_SQL_DIR="$SCRIPT_DIR/burst_sql"

[[ -x "$BURST" || -r "$BURST" ]] || { echo "FAIL: $BURST not found"; exit 1; }

pass=0
fail=0
ok()   { echo "  PASS  $*"; pass=$((pass + 1)); }
bad()  { echo "  FAIL  $*"; fail=$((fail + 1)); }

WORK=$(mktemp -d "${TMPDIR:-/tmp}/pgwt_burst_guards_XXXXXX")
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

# ── Stubs ───────────────────────────────────────────────────────────────────
BIN="$WORK/bin"
mkdir -p "$BIN"

# Fake clock: the stub `sleep` advances it, the stub `date +%s` reads it. Any
# test that cares about the duration budget sets PGWT_T_CLOCK_START and reads
# nothing from the real wall clock, so "did the cycle finish inside the
# budget" is decided by arithmetic, not by how loaded this Mac is.
cat >"$BIN/sleep" <<'EOF'
#!/bin/bash
# PGWT_T_SLEEP_MULT: advance the fake clock by n x mult instead of n. The
# stub bursts are instantaneous, so the clock only ever moves by the PAUSES
# -- which means no duration that passes the scale derivation could overrun
# its own cycle, and the `nocycle` guard would be unreachable in this suite.
# A multiplier models "the box was slower than the pauses assume" without
# making anything depend on real wall time.
n=${1:-0}
n=${n%%.*}
cur=$(cat "$PGWT_T_CLOCK" 2>/dev/null || echo 0)
echo $(( cur + ${n:-0} * ${PGWT_T_SLEEP_MULT:-1} )) > "$PGWT_T_CLOCK"
exit 0
EOF

cat >"$BIN/date" <<'EOF'
#!/bin/bash
if [[ "${1:-}" == "+%s" ]]; then
    cat "$PGWT_T_CLOCK" 2>/dev/null || echo 0
    exit 0
fi
exec /bin/date "$@"
EOF

# psql stub. Called as: psql -U postgres -p PORT -d postgres -Atc "QUERY".
# Answers only the four queries burst_load.sh actually issues; anything else
# is an error, so a new query added to the script without a stub answer shows
# up as a loud failure here rather than as a silently empty string.
cat >"$BIN/psql" <<'EOF'
#!/bin/bash
q=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        -Atc) q="$2"; shift 2 ;;
        *) shift ;;
    esac
done
case "$q" in
    CREATE\ TABLE*pgwt_burst*) exit 0 ;;
    *pg_class*relname*pgwt_burst*) echo "${PGWT_T_BURST_TBL-1}"; exit 0 ;;
    *count\(\*\)\ FROM\ _smoke_io_load*) echo "${PGWT_T_IO_ROWS-3000000}"; exit 0 ;;
    *n_tup_ins*pgwt_burst*)
        f="$PGWT_T_INS"
        cur=$(cat "$f" 2>/dev/null || echo 0)
        # First call of a pair returns the current value, second call returns
        # current + delta -- i.e. the burst "inserted" PGWT_T_INS_DELTA rows.
        # Delta 0 reproduces "pgbench said OK, nothing reached the table".
        side=$(cat "$f.side" 2>/dev/null || echo before)
        if [[ "$side" == "before" ]]; then
            echo after > "$f.side"
            echo "${PGWT_T_INS_BEFORE-$cur}"
        else
            echo before > "$f.side"
            echo $(( cur + ${PGWT_T_INS_DELTA-1000} )) > "$f"
            echo "${PGWT_T_INS_AFTER-$(cat "$f")}"
        fi
        exit 0 ;;
esac
echo "psql stub: unexpected query: $q" >&2
exit 1
EOF

# pgbench stub. Emits a real pgbench summary. Per-tag overrides key off
# PGAPPNAME (which burst_load.sh sets to the burst tag) so one burst can be
# broken while the others stay healthy.
cat >"$BIN/pgbench" <<'EOF'
#!/bin/bash
tag="${PGAPPNAME:-unknown}"
rc_var="PGWT_T_RC_${tag}"; rc="${!rc_var-${PGWT_T_RC-0}}"
tps_var="PGWT_T_TPS_${tag}"; tps="${!tps_var-${PGWT_T_TPS-1000}}"
ntx_var="PGWT_T_NTX_${tag}"; ntx="${!ntx_var-${PGWT_T_NTX-4800}}"
mode_var="PGWT_T_MODE_${tag}"; mode="${!mode_var-${PGWT_T_MODE-normal}}"
if [[ "$mode" == "nosummary" ]]; then
    # pgbench that ran but printed no parseable summary (a future version
    # changing its output, a crash after the run, a truncated pipe).
    echo "pgbench (18.0)"
    exit "$rc"
fi
cat <<SUMMARY
pgbench (18.0)
transaction type: multiple scripts
scaling factor: 10
number of clients: 6
number of threads: 2
number of transactions actually processed: $ntx/$ntx
tps = $tps.123456 (without initial connection time)
SUMMARY
exit "$rc"
EOF

chmod +x "$BIN"/*

# ── Fixture SQL directories ─────────────────────────────────────────────────
mk_sql_dir() {
    # mk_sql_dir DEST [--no-sentinel] [--empty FILE] [--drop FILE]
    local dest="$1"; shift
    mkdir -p "$dest"
    cp "$REAL_SQL_DIR"/*.sql "$dest"/
    cp "$REAL_SQL_DIR/.not-an-rsync-target" "$dest"/
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --no-sentinel) rm -f "$dest/.not-an-rsync-target"; shift ;;
            --empty) : > "$dest/$2"; shift 2 ;;
            --drop) rm -f "$dest/$2"; shift 2 ;;
            *) echo "mk_sql_dir: bad arg $1" >&2; exit 1 ;;
        esac
    done
}

# ── Runner ──────────────────────────────────────────────────────────────────
# run_case SCRIPT SQL_DIR DURATION [env assignments...]
# Echoes "RC=<n>" then the combined output.
run_case() {
    local script="$1" sqldir="$2" dur="$3"; shift 3
    local clock="$WORK/clock.$RANDOM"
    local ins="$WORK/ins.$RANDOM"
    echo "${PGWT_T_CLOCK_START:-1000}" >"$clock"
    echo 0 >"$ins"
    echo before >"$ins.side"
    # PGPORT must be settable to the empty string by a caller (that is the
    # pgport guard's own input), so it is only defaulted when the caller did
    # not name it among the env assignments in "$@". Defaulting it via
    # ${PGPORT_OVERRIDE-...} here would not work: the assignments in "$@" are
    # applied by `env` to the CHILD, and are not visible to this expansion.
    local a has_pgport=0
    for a in "$@"; do
        [[ "$a" == PGPORT=* ]] && has_pgport=1
    done
    local pgport_arg=()
    [[ $has_pgport -eq 0 ]] && pgport_arg=(PGPORT=5432)
    local out rc
    out=$(env PATH="$BIN:$PATH" \
        PGWT_T_CLOCK="$clock" PGWT_T_INS="$ins" \
        PGWT_BURST_SQL_DIR="$sqldir" \
        PGWT_BURST_LOG="$WORK/burst.log.$RANDOM" \
        PGWT_BURST_CYCLES="${PGWT_BURST_CYCLES:-1}" \
        "${pgport_arg[@]}" \
        "$@" bash "$script" "$dur" 2>&1)
    rc=$?
    echo "RC=$rc"
    echo "$out"
}

# expect_refusal LABEL SIGNATURE SCRIPT SQL_DIR DURATION [env...]
expect_refusal() {
    local label="$1" sig="$2"; shift 2
    local res rc
    res=$(run_case "$@")
    rc=$(sed -n '1s/^RC=//p' <<<"$res")
    local body
    body=$(tail -n +2 <<<"$res")
    if [[ "$rc" == "0" ]]; then
        bad "$label: expected a refusal, got exit 0"
        echo "$body" | sed 's/^/        /' | tail -n 6
        return
    fi
    if ! grep -qF -- "$sig" <<<"$body"; then
        bad "$label: refused (rc=$rc) but without its own signature '$sig'"
        echo "$body" | sed 's/^/        /' | tail -n 8
        return
    fi
    ok "$label (rc=$rc, signature present)"
}

# ── Phase A: every guard's true positive, plus the happy path ───────────────
echo "Phase A -- each guard refuses its own broken input"

mk_sql_dir "$WORK/sql_good"
mk_sql_dir "$WORK/sql_nosentinel" --no-sentinel
mk_sql_dir "$WORK/sql_emptyfile" --empty burst.sql
mk_sql_dir "$WORK/sql_droppedfile" --drop io_read.sql

# The happy path FIRST: if this cannot pass, every refusal below is vacuous.
res=$(run_case "$BURST" "$WORK/sql_good" 600)
rc=$(sed -n '1s/^RC=//p' <<<"$res")
if [[ "$rc" == "0" ]] && grep -q "first cycle OK" <<<"$res"; then
    ok "happy path: one full cycle, exit 0, 'first cycle OK' logged"
else
    bad "happy path: rc=$rc (expected 0 with 'first cycle OK')"
    tail -n 20 <<<"$res" | sed 's/^/        /'
fi

expect_refusal "usage: no DURATION_S" \
    "usage:" "$BURST" "$WORK/sql_good" ""
expect_refusal "usage: non-numeric DURATION_S" \
    "usage:" "$BURST" "$WORK/sql_good" "abc"

expect_refusal "pgport: PGPORT empty" \
    "PGPORT is unset or not numeric" "$BURST" "$WORK/sql_good" 600 \
    PGPORT=
expect_refusal "pgport: PGPORT non-numeric" \
    "PGPORT is unset or not numeric" "$BURST" "$WORK/sql_good" 600 \
    PGPORT=not-a-port

# tooling: a PATH on which pgbench genuinely does not exist. PATH is set to
# ONE directory holding the stubs plus symlinks to exactly the externals
# burst_load.sh needs before/at the tooling guard -- rather than
# "$NOPGB:/usr/bin:/bin", because on a Linux gate box /usr/bin/pgbench does
# exist and the case would then silently stop testing anything. bash itself
# is invoked by absolute path for the same reason.
NOPGB="$WORK/nopgbench"
mkdir -p "$NOPGB"
cp "$BIN/psql" "$BIN/sleep" "$BIN/date" "$NOPGB"/
for t in dirname mktemp tee pkill pgrep cat sed awk md5sum md5; do
    p=$(command -v "$t" 2>/dev/null) && ln -sf "$p" "$NOPGB/$t"
done
if command -v pgbench >/dev/null 2>&1 && [[ -e "$NOPGB/pgbench" ]]; then
    bad "tooling: fixture PATH still contains pgbench -- the case cannot test anything"
fi
res=$(env -i PATH="$NOPGB" PGWT_T_CLOCK="$WORK/c1" PGWT_T_INS="$WORK/i1" \
    PGWT_BURST_SQL_DIR="$WORK/sql_good" PGWT_BURST_LOG="$WORK/l1" \
    PGWT_BURST_CYCLES=1 PGPORT=5432 /bin/bash "$BURST" 600 2>&1)
rc=$?
if [[ "$rc" != "0" ]] && grep -q "pgbench not found on PATH" <<<"$res"; then
    ok "tooling: pgbench absent from PATH refuses (rc=$rc)"
else
    bad "tooling: pgbench absent from PATH -- rc=$rc, output: $(tail -n 3 <<<"$res")"
fi

expect_refusal "sentinel: .not-an-rsync-target deleted" \
    "is not an intact checkout of tests/burst_sql" \
    "$BURST" "$WORK/sql_nosentinel" 600
expect_refusal "sqlfiles: burst.sql truncated to empty" \
    "missing, empty or unreadable" "$BURST" "$WORK/sql_emptyfile" 600
expect_refusal "sqlfiles: io_read.sql deleted (the 2026-10-02 input)" \
    "missing, empty or unreadable" "$BURST" "$WORK/sql_droppedfile" 600

expect_refusal "iorows: _smoke_io_load empty (absent, not wrong)" \
    "want >= 3000000" "$BURST" "$WORK/sql_good" 600 PGWT_T_IO_ROWS=0
expect_refusal "iorows: _smoke_io_load short" \
    "want >= 3000000" "$BURST" "$WORK/sql_good" 600 PGWT_T_IO_ROWS=1000
expect_refusal "iorows: row count unreadable" \
    "want >= 3000000" "$BURST" "$WORK/sql_good" 600 PGWT_T_IO_ROWS=

expect_refusal "rc: a burst exits non-zero" \
    "b_wal exited 3" "$BURST" "$WORK/sql_good" 600 PGWT_T_RC_b_wal=3
expect_refusal "rc: a burst exits 127 (command not found)" \
    "BROKEN INVOCATION" "$BURST" "$WORK/sql_good" 600 PGWT_T_RC_b_lm=127
expect_refusal "rc: a burst exits 126 (not executable)" \
    "BROKEN INVOCATION" "$BURST" "$WORK/sql_good" 600 PGWT_T_RC_b_ior=126

expect_refusal "parse: pgbench exits 0 with no parseable summary" \
    "could not parse achieved tps" "$BURST" "$WORK/sql_good" 600 \
    PGWT_T_MODE_b_wal=nosummary

expect_refusal "floor: achieved tps under the per-burst floor" \
    "below floor 100" "$BURST" "$WORK/sql_good" 600 PGWT_T_TPS_b_wal=3
expect_refusal "ntx: zero transactions processed" \
    "processed 0 transactions" "$BURST" "$WORK/sql_good" 600 PGWT_T_NTX_b_wal=0

expect_refusal "walrows: pgbench OK but nothing reached pgwt_burst" \
    "nothing reached the table" "$BURST" "$WORK/sql_good" 600 \
    PGWT_T_INS_DELTA=0
expect_refusal "walparse: n_tup_ins unreadable" \
    "refusing to approve a WAL burst whose end-to-end proof" \
    "$BURST" "$WORK/sql_good" 600 PGWT_T_INS_BEFORE= PGWT_T_INS_AFTER=

# scale: a duration shorter than even one scale-1 cycle is refused UP FRONT,
# before any burst runs -- cheaper and clearer than discovering it after
# burning a partial cycle.
expect_refusal "scale: duration shorter than one base cycle" \
    "shorter than even one scale-1 cycle" \
    "$BURST" "$WORK/sql_good" 10

# nocycle: the duration passes the scale derivation (138s fits one scale-1
# cycle) but the cycle then overruns it anyway -- a box slower than the pauses
# assume. Pinned by the fake clock via PGWT_T_SLEEP_MULT=5, so this is
# arithmetic rather than a race: the first 32 s pause advances the clock 160 s
# past a 138 s budget. Without this case the `nocycle` guard is unreachable in
# this suite and its Phase A pass would prove nothing.
expect_refusal "nocycle: cycle overran a duration that the scale fitted" \
    "expired before a single cycle completed" \
    "$BURST" "$WORK/sql_good" 138 PGWT_T_SLEEP_MULT=5

# ── Phase B: each guard is REACHABLE and is what produces its refusal ───────
echo
echo "Phase B -- deleting each guard removes its refusal (reachability)"

# mutant NAME -> a copy of burst_load.sh with that guard's block removed.
mutant() {
    local name="$1" dest="$WORK/mutant_$1.sh"
    awk -v g="$name" '
        $0 == "# GUARD-BEGIN:" g { skip = 1; next }
        $0 == "# GUARD-END:" g   { skip = 0; found = 1; next }
        !skip { print }
        END { if (!found) exit 9 }
    ' "$BURST" >"$dest" || {
        bad "mutant $name: no GUARD-BEGIN/END:$name markers in burst_load.sh"
        return 1
    }
    echo "$dest"
}

# expect_bypass LABEL GUARD SIGNATURE SQL_DIR DURATION [env...]
expect_bypass() {
    local label="$1" guard="$2" sig="$3"; shift 3
    local m
    m=$(mutant "$guard") || return
    local res rc body
    res=$(run_case "$m" "$@")
    rc=$(sed -n '1s/^RC=//p' <<<"$res")
    body=$(tail -n +2 <<<"$res")
    if grep -qF -- "$sig" <<<"$body"; then
        bad "$label: guard '$guard' deleted but its refusal still appears -- the marked block is not what produces it, so Phase A proved nothing about this guard"
        echo "$body" | sed 's/^/        /' | tail -n 6
        return
    fi
    if [[ "$rc" == "0" ]]; then
        ok "$label: without guard '$guard' the broken input is APPROVED (exit 0) -- 2026-10-02 reproduced"
    else
        ok "$label: without guard '$guard' the refusal is gone (exit $rc, different cause)"
    fi
}

expect_bypass "sentinel deleted" sentinel \
    "is not an intact checkout of tests/burst_sql" "$WORK/sql_nosentinel" 600
expect_bypass "sqlfiles deleted" sqlfiles \
    "missing, empty or unreadable" "$WORK/sql_droppedfile" 600
expect_bypass "iorows deleted" iorows \
    "want >= 3000000" "$WORK/sql_good" 600 PGWT_T_IO_ROWS=0
expect_bypass "rc deleted" rc \
    "b_wal exited 3" "$WORK/sql_good" 600 PGWT_T_RC_b_wal=3
expect_bypass "rc deleted (127)" rc \
    "BROKEN INVOCATION" "$WORK/sql_good" 600 PGWT_T_RC_b_lm=127
expect_bypass "parse deleted" parse \
    "could not parse achieved tps" "$WORK/sql_good" 600 PGWT_T_MODE_b_wal=nosummary
expect_bypass "floor deleted" floor \
    "below floor 100" "$WORK/sql_good" 600 PGWT_T_TPS_b_wal=3
expect_bypass "ntx deleted" ntx \
    "processed 0 transactions" "$WORK/sql_good" 600 PGWT_T_NTX_b_wal=0
expect_bypass "walrows deleted" walrows \
    "nothing reached the table" "$WORK/sql_good" 600 PGWT_T_INS_DELTA=0
expect_bypass "walparse deleted" walparse \
    "refusing to approve a WAL burst whose end-to-end proof" "$WORK/sql_good" 600 \
    PGWT_T_INS_BEFORE= PGWT_T_INS_AFTER=
expect_bypass "nocycle deleted" nocycle \
    "expired before a single cycle completed" "$WORK/sql_good" 138 \
    PGWT_T_SLEEP_MULT=5
# Deleting the scale guard leaves BURST_SCALE=0, so every -T becomes 0 and
# every pause becomes 0: the stub bursts still "succeed" and the run reports a
# completed cycle. A real pgbench -T 0 does no work at all, so this mutant is
# the shape of "the burst tier ran and measured nothing" being APPROVED.
expect_bypass "scale deleted" scale \
    "shorter than even one scale-1 cycle" "$WORK/sql_good" 10
expect_bypass "pgport deleted" pgport \
    "PGPORT is unset or not numeric" "$WORK/sql_good" 600 PGPORT=
# tooling's bypass case cannot go through run_case (which puts the full
# stub PATH, pgbench included, in front) -- it needs the same restricted
# PATH as the Phase A case above.
if m=$(mutant tooling); then
    res=$(env -i PATH="$NOPGB" PGWT_T_CLOCK="$WORK/c2" PGWT_T_INS="$WORK/i2" \
        PGWT_BURST_SQL_DIR="$WORK/sql_good" PGWT_BURST_LOG="$WORK/l2" \
        PGWT_BURST_CYCLES=1 PGPORT=5432 /bin/bash "$m" 600 2>&1)
    rc=$?
    if grep -q "pgbench not found on PATH" <<<"$res"; then
        bad "tooling deleted: guard 'tooling' removed but its refusal still appears"
    else
        ok "tooling deleted: without guard 'tooling' the refusal is gone (exit $rc, bare 127 from inside the cycle loop instead of a named diagnosis)"
    fi
fi

# ── Phase C: the connection-reuse scale derivation ─────────────────────────
#
# The 2026-10-08 fix for paint and for the blink self-test is connection reuse:
# every burst keeps its client count and its --rate, but runs BURST_SCALE times
# longer with BURST_SCALE times longer pauses, so the same work arrives from
# BURST_SCALE times fewer backend generations. Two things have to hold or that
# claim is false:
#
#   1. The scale is DERIVED from DURATION_S, never configured -- a knob would
#      be a way to change the load silently. It must pick the largest scale
#      that still completes a cycle, cap at the scale whose cycle fits the UI's
#      900 s window, and REFUSE (0) when not even scale 1 fits, rather than
#      clamping to 1 and running a cycle the caller has no time for.
#   2. Transactions per second averaged over a cycle must be IDENTICAL at
#      every scale. If it is not, "connection reuse" quietly became "thinner
#      load", which is the one thing the owner ruled out. This is arithmetic,
#      so it is checked as arithmetic rather than asserted in a comment.
echo
echo "Phase C -- scale derivation and load invariance"

# burst_scale_for is a function inside burst_load.sh, which exits early without
# PGPORT, so it is extracted and called in a subshell. Every constant it needs
# is read OUT OF THE SCRIPT, never re-declared here. An earlier version of this
# helper declared `BURST_SCALE_MAX=6` itself, which meant capping the script's
# own BURST_SCALE_MAX to 1 -- the entire connection-reuse change reverted --
# still passed every case in this file. A suite that supplies the value it is
# meant to be checking cannot fail.
grab() {   # grab NAME -> the script's own assignment, or empty
    # Not anchored at line start: burst_load.sh puts two constants on one line
    # ("B_WAL_T=8;  B_WAL_PAUSE=32"), and an anchored pattern silently matched
    # nothing for the second of each pair. The "could not read" refusal below
    # is what caught that, instead of this suite inventing a value -- which is
    # the whole reason that refusal is there.
    grep -oE "(^|[;[:space:]])$1=[0-9]+" "$BURST" | head -1 | grep -oE '[0-9]+$'
}
SCRIPT_MAX=$(grab BURST_SCALE_MAX)
SCRIPT_WAL_T=$(grab B_WAL_T);  SCRIPT_WAL_P=$(grab B_WAL_PAUSE)
SCRIPT_LM_T=$(grab B_LM_T);    SCRIPT_LM_P=$(grab B_LM_PAUSE)
SCRIPT_IO_T=$(grab B_IO_T);    SCRIPT_IO_P=$(grab B_IO_PAUSE)
for v in SCRIPT_MAX SCRIPT_WAL_T SCRIPT_WAL_P SCRIPT_LM_T SCRIPT_LM_P SCRIPT_IO_T SCRIPT_IO_P; do
    if [[ -n "${!v}" ]]; then :; else
        bad "could not read $v out of burst_load.sh -- refusing to test against values this suite invented"
    fi
done
BASE=$(( SCRIPT_WAL_T + SCRIPT_WAL_P + SCRIPT_LM_T + SCRIPT_LM_P + SCRIPT_IO_T + SCRIPT_IO_P ))

scale_for() {
    bash -c '
        '"$(sed -n '/^burst_scale_for() {/,/^}/p' "$BURST")"'
        BURST_SCALE_MAX='"$SCRIPT_MAX"'
        BASE_CYCLE_S='"$BASE"'
        burst_scale_for "$1" "${2:-$BURST_SCALE_MAX}" "${3:-$BASE_CYCLE_S}"
    ' _ "$@"
}

if [[ "$BASE" == "138" ]]; then
    ok "base cycle read from burst_load.sh is ${BASE}s (8+32+1+37+6+54)"
else
    bad "base cycle read from burst_load.sh is ${BASE}s, this suite's table assumes 138s"
fi
# THE CAP ITSELF, read from the script: 6 x 138 = 828 <= 900 < 7 x 138 = 966.
# This is what catches the cap being lowered (no reuse) or raised (a cycle
# longer than the UI window).
if [[ "$SCRIPT_MAX" == "6" ]]; then
    ok "burst_load.sh's own BURST_SCALE_MAX is 6 (cycle $(( 6 * BASE ))s <= the 900s UI window)"
else
    bad "burst_load.sh's own BURST_SCALE_MAX is $SCRIPT_MAX, not 6 -- at $SCRIPT_MAX the cycle is $(( SCRIPT_MAX * BASE ))s; below 6 the backend churn this change exists to remove comes back, above 6 a 900s window can contain no WAL burst"
fi

expect_scale() {
    local dur="$1" want="$2" why="$3" got
    got=$(scale_for "$dur")
    if [[ "$got" == "$want" ]]; then
        ok "DURATION_S=$dur -> scale $got ($why)"
    else
        bad "DURATION_S=$dur -> scale $got, expected $want ($why)"
    fi
}

# The cap: 6 x 138 = 828 <= 900 (the UI window); 7 x 138 = 966 > 900.
expect_scale 1800 6 "the live-UI-smoke walk: churn-minimising scale, cycle 828s"
expect_scale 2100 6 "demo_rehearsal default 35min: capped at 6, not 15"
expect_scale 86400 6 "a very long run is still capped at 6 (window coverage)"
expect_scale 828  6 "exactly one scale-6 cycle fits"
expect_scale 827  5 "one second short of a scale-6 cycle -> scale 5 (690s)"
expect_scale 690  5 "exactly one scale-5 cycle"
expect_scale 552  4 "exactly one scale-4 cycle"
expect_scale 414  3 "exactly one scale-3 cycle"
expect_scale 276  2 "exactly one scale-2 cycle"
expect_scale 180  1 "demo_rehearsal.sh's documented DURATION_MIN=3 self-test"
expect_scale 138  1 "exactly one base cycle"
# The refusal, not a clamp: scale 0 is the signal burst_load.sh dies on.
expect_scale 137  0 "one second short of a base cycle -> REFUSE, never clamp to 1"
expect_scale 1    0 "absurdly short -> REFUSE"

# Monotonic: a longer run must never get a SMALLER scale (fewer backends is
# always available to a longer run). A non-monotonic derivation would mean some
# durations silently churn more than shorter ones do.
prev=0
nonmono=""
for d in 100 138 200 276 300 414 500 552 600 690 700 828 900 1200 1800 3600; do
    cur=$(scale_for "$d")
    [[ "$cur" -lt "$prev" ]] && nonmono="$nonmono ${d}s($cur<$prev)"
    prev=$cur
done
if [[ -z "$nonmono" ]]; then
    ok "scale is monotonic non-decreasing in DURATION_S"
else
    bad "scale DECREASES as duration grows at:$nonmono"
fi

# Every derived scale must actually fit at least one cycle -- a scale whose
# cycle exceeds the duration is the bug the refusal exists to prevent.
badfit=""
for d in 138 180 276 414 552 690 828 900 1800 2100; do
    cur=$(scale_for "$d")
    [[ $(( cur * BASE )) -le "$d" ]] || badfit="$badfit ${d}s(scale $cur)"
done
if [[ -z "$badfit" ]]; then
    ok "every derived scale completes at least one cycle within its duration"
else
    bad "derived a scale that cannot complete a cycle:$badfit"
fi

# Cap respected for real: no duration may yield a cycle longer than the 900 s
# UI window, or a window can contain no WAL burst at all.
overwin=""
for d in 828 900 1800 2100 86400; do
    cur=$(scale_for "$d")
    [[ $(( cur * BASE )) -le 900 ]] || overwin="$overwin ${d}s(cycle $(( cur * BASE ))s)"
done
if [[ -z "$overwin" ]]; then
    ok "no duration yields a cycle longer than the 900s UI window"
else
    bad "cycle exceeds the 900s window for:$overwin"
fi

# A custom max is honoured (so the cap is a parameter of the derivation, not
# baked into its body -- otherwise the cases above cannot distinguish a real
# cap from a coincidence).
got=$(scale_for 86400 3)
if [[ "$got" == "3" ]]; then
    ok "an explicit max is honoured (max=3 -> scale 3, not 6)"
else
    bad "an explicit max is ignored: max=3 gave scale $got"
fi

# ── Load invariance: the whole point of the change ─────────────────────────
# Per-cycle transactions = rate x duration; per-cycle seconds = scale x base.
# tx/s averaged over a cycle must not move with scale. Checked for each burst
# with its own rate, in integer hundredths to avoid shell float arithmetic.
invariance_fail=""
for spec in "600:8:b_wal" "300:6:b_ior" "50:6:b_iow"; do
    rate=${spec%%:*}; rest=${spec#*:}; dur=${rest%%:*}; tag=${rest##*:}
    ref=""
    for (( sc = 1; sc <= SCRIPT_MAX; sc++ )); do
        # (rate * dur * sc * 100) / (base * sc)  == (rate * dur * 100) / base
        v=$(( (rate * dur * sc * 100) / (BASE * sc) ))
        [[ -z "$ref" ]] && ref=$v
        [[ "$v" == "$ref" ]] || invariance_fail="$invariance_fail $tag@scale$sc($v!=$ref)"
    done
    ok "$tag average load is ${ref} tx/s x100 at every scale 1..$SCRIPT_MAX"
done
if [[ -n "$invariance_fail" ]]; then
    bad "average load CHANGES with scale (the load would be thinned):$invariance_fail"
fi

# BEHAVIOURAL check that the PAUSES really scale, not just the durations.
# The arithmetic above is over constants, so it cannot see a script that scales
# `-T` but leaves `isleep` at its base value -- which would make the load 6x
# THICKER, the opposite of the intent and exactly the kind of silent change the
# owner ruled out. The fake clock only advances when the script sleeps, so the
# clock delta across one completed cycle IS the sum of that cycle's pauses.
# (With PGWT_BURST_CYCLES=1 the trailing pause is never reached, so the delta
# is the first two pauses: 32+37 at scale 1, and 6x that at scale 6.)
cycle_pause_delta() {   # cycle_pause_delta DURATION_S -> fake-clock delta
    local dur="$1"
    local clock="$WORK/clock.beh.$RANDOM" ins="$WORK/ins.beh.$RANDOM"
    echo 1000 >"$clock"; echo 0 >"$ins"; echo before >"$ins.side"
    env PATH="$BIN:$PATH" PGWT_T_CLOCK="$clock" PGWT_T_INS="$ins" \
        PGWT_BURST_SQL_DIR="$WORK/sql_good" PGWT_BURST_LOG="$WORK/beh.log.$RANDOM" \
        PGWT_BURST_CYCLES=1 PGPORT=5432 \
        bash "$BURST" "$dur" >/dev/null 2>&1
    echo $(( $(cat "$clock") - 1000 ))
}
d1=$(cycle_pause_delta "$BASE")
d6=$(cycle_pause_delta $(( SCRIPT_MAX * BASE )))
want1=$(( SCRIPT_WAL_P + SCRIPT_LM_P ))
want6=$(( SCRIPT_MAX * want1 ))
if [[ "$d1" == "$want1" ]]; then
    ok "one scale-1 cycle slept ${d1}s of pauses (= ${SCRIPT_WAL_P}+${SCRIPT_LM_P})"
else
    bad "one scale-1 cycle slept ${d1}s of pauses, expected ${want1}s"
fi
if [[ "$d6" == "$want6" ]]; then
    ok "one scale-$SCRIPT_MAX cycle slept ${d6}s of pauses (= $SCRIPT_MAX x ${want1}) -- pauses scale with the durations, so average load is unchanged"
else
    bad "one scale-$SCRIPT_MAX cycle slept ${d6}s of pauses, expected ${want6}s -- the PAUSES are not scaling with the durations, so the load is $(( d1 > 0 ? want6 / (d6 > 0 ? d6 : 1) : 0 ))x thicker than intended, not merely reusing connections"
fi

# Backend generations must actually FALL with scale -- otherwise the change
# achieves nothing. 19 clients per cycle, over a 1800 s run.
g1=$(( (1800 + BASE - 1) / BASE * 19 ))
g6=$(( (1800 + SCRIPT_MAX * BASE - 1) / (SCRIPT_MAX * BASE) * 19 ))
if [[ "$g6" -lt "$g1" ]]; then
    ok "backend generations over 1800s fall from $g1 (scale 1) to $g6 (scale $SCRIPT_MAX)"
else
    bad "backend generations did not fall: scale1=$g1 scale$SCRIPT_MAX=$g6 -- the change achieves nothing"
fi

echo
echo "burst_load guards: $pass passed, $fail failed"
[[ $fail -eq 0 ]] || exit 1
