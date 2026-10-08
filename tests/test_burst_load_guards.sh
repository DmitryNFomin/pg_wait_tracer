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
n=${1:-0}
n=${n%%.*}
cur=$(cat "$PGWT_T_CLOCK" 2>/dev/null || echo 0)
echo $(( cur + ${n:-0} )) > "$PGWT_T_CLOCK"
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

# nocycle: the duration budget expires mid-cycle. Pinned by the fake clock
# (the first inter-burst pause advances it 32 s past a 10 s budget), so this
# case is arithmetic, not a race.
expect_refusal "nocycle: budget expired before one cycle completed" \
    "expired before a single cycle completed" \
    "$BURST" "$WORK/sql_good" 10

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
    "expired before a single cycle completed" "$WORK/sql_good" 10
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

echo
echo "burst_load guards: $pass passed, $fail failed"
[[ $fail -eq 0 ]] || exit 1
