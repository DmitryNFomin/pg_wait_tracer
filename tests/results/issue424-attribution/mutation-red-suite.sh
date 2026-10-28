#!/usr/bin/env bash
# mutation-red-suite.sh — every way §10 of tests/test_open_cpu_accounting.c
# (open_cpu_io_worker_refused_total, open_cpu_unplaced_total) could be made
# UNREACHABLE or VACUOUS, and the proof that each one turns it red.
#
# Showing a gate can go red proves the true-positive path, and that has never
# been the broken one here. The defects that keep getting through are false
# NEGATIVES: a counter whose two sides come from the same place, a check
# satisfied without checking anything, a section that is not dispatched, a
# guard broadened until the mutant it was written for is equivalent. So each
# mutant below is one of those, applied to src/summary_writer.c (or to the
# test's own main()), and the §10 line it must kill is named.
#
# TWO of them are the shapes that would have been WORSE than the defect:
#   S1 counts every retirement, so a HEALTHY credited capture reads
#      unplaced > 0 and the live fixture REFUSES it -- the gate turns itself
#      off while reporting a reason.
#   S7 counts at the note, so both sides of the live fixture's reading come
#      from the same event and "accepted then retired unplaced" can no longer
#      be told from "offered and never accepted".
#
# Runs on macOS or Linux with no BPF and no PostgreSQL: the test is a server
# build. lz4 headers are taken from `brew --prefix lz4` when present.
#
# Usage: tests/results/issue424-attribution/mutation-red-suite.sh
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/v9attrmutXXXXXX")"
trap 'rm -rf "$WORK"' EXIT
cd "$REPO" || exit 1

EXTRA_INC=""
EXTRA_LIB=""
if command -v brew >/dev/null 2>&1 && p=$(brew --prefix lz4 2>/dev/null); then
    EXTRA_INC="-I$p/include"; EXTRA_LIB="-L$p/lib"
fi
# macOS has no pipe2(); src/spawn.c is only linked in, never exercised here.
PIPE2_SHIM=()
if [[ "$(uname -s)" == "Darwin" ]]; then
    PIPE2_SHIM=('-Dpipe2(a,b)=pipe(a)')
fi

SUPPORT=(src/summary_reader.c src/event_writer.c src/event_reader.c
         src/wait_event.c src/idle_rule.c src/spawn.c src/cJSON.c)

# build <test-c> <writer-c> <out>
build() {
    ${CC:-cc} -g -O1 -w -DPGWT_SERVER -DPGWT_BUILD_VERSION='"mutant"' \
       -Iinclude -Isrc $EXTRA_INC "${PIPE2_SHIM[@]+"${PIPE2_SHIM[@]}"}" \
       "$1" "$2" "${SUPPORT[@]}" \
       $EXTRA_LIB -llz4 -lz -lm -o "$3" 2>"$WORK/cc.err"
}

TEST=tests/test_open_cpu_accounting.c
WRITER=src/summary_writer.c

run_one() {
    local label="$1" test_c="$2" writer_c="$3" must_kill="$4"
    echo ""
    echo "=== MUTANT: $label"
    echo "    must turn red: $must_kill"
    if ! build "$test_c" "$writer_c" "$WORK/bin"; then
        echo "  COMPILE FAILED (a red, but not an assertion red):"
        /usr/bin/grep -m3 error "$WORK/cc.err" | sed 's/^/    /'
        return
    fi
    "$WORK/bin" > "$WORK/out.txt" 2>&1
    local rc=$?
    sed -n '/-- 10:/,$p' "$WORK/out.txt" | /usr/bin/grep -E 'FAIL:|checks,' \
        | sed 's/^/  /'
    echo "  exit=$rc"
}

echo "### BASELINE (unmutated) — all of §10 must be green ###"
build "$TEST" "$WRITER" "$WORK/base" || { echo "baseline build failed"; cat "$WORK/cc.err"; exit 1; }
"$WORK/base" > "$WORK/base.txt" 2>&1
echo "  exit=$?"
sed -n '/-- 10:/,$p' "$WORK/base.txt" | /usr/bin/grep -E 'ok:|FAIL:|checks,' \
    | sed 's/^/  /'

# S1 — unplaced counted for EVERY retirement, not only an unwritten one.
# The cheaper edit, and the damaging one: a healthy multi-second gap is
# credited and then closed, so it would read unplaced > 0 and the live
# fixture would REFUSE ("no gap to spread") on a capture that spread one.
sed 's/    if (o->cpu_slot && !o->cpu_frozen)/    if (o->cpu_slot)/' \
    "$WRITER" > "$WORK/s1.c"
run_one "S1: counted for every retirement (the cpu_frozen test dropped)" \
        "$TEST" "$WORK/s1.c" "10b"

# S2 — the unplaced increment deleted. The counter reads 0 forever, so the
# live fixture falls through to FAIL and names src/summary_writer.c on a
# capture whose gaps simply ended inside their own second: the 2026-10-28
# misdiagnosis, restored.
sed 's/^        w->open_cpu_unplaced_total++;/        (void)0;/' \
    "$WRITER" > "$WORK/s2.c"
run_one "S2: unplaced increment deleted (counter permanently 0)" \
        "$TEST" "$WORK/s2.c" "10a, 10e"

# S3 — the io_worker CPU subset counted for every io_worker note, WAIT notes
# included. It stops being a subset of notes_total, so the live fixture's
# "offered = notes - subset" goes negative and its arithmetic inverts.
sed 's/^        if (on_cpu_note && cpu)\n            w->open_cpu_io_worker_refused_total++;/X/' "$WRITER" > /dev/null
awk '
  /w->open_cpu_io_worker_refused_total\+\+;/ { prev_is_guard = 1 }
  /if \(on_cpu_note && cpu\)$/ && !seen_guard_once { guard_line = NR }
  { lines[NR] = $0 }
  END {
    for (i = 1; i <= NR; i++) {
      if (lines[i] ~ /if \(on_cpu_note && cpu\)$/ && lines[i+1] ~ /open_cpu_io_worker_refused_total/) {
        sub(/if \(on_cpu_note && cpu\)/, "if (1)", lines[i])
      }
      print lines[i]
    }
  }' "$WRITER" > "$WORK/s3.c"
run_one "S3: the subset counted for WAIT notes too (no longer a subset)" \
        "$TEST" "$WORK/s3.c" "10c, 10d"

# S4 — the io_worker CPU subset increment deleted. The overlap with
# open_io_worker_refused_total becomes unknown again, which is exactly the
# pair (34 notes, 39 refusals) that could not be read on the gate box.
sed 's/^            w->open_cpu_io_worker_refused_total++;/            (void)0;/' \
    "$WRITER" > "$WORK/s4.c"
run_one "S4: the subset increment deleted (the overlap unknown again)" \
        "$TEST" "$WORK/s4.c" "10c"

# S5 — THE DRIFT MUTANT. A second, uncounted way to retire an interval,
# used by one caller. This is why the writer is a PARAMETER of
# open_stop_crediting rather than an optional extra: one site, or the
# outcome goes missing for whichever path forgets.
sed 's|^static void open_stop_crediting(struct pgwt_summary_writer \*w,|static void open_stop_crediting_uncounted(struct pgwt_summary_open *o) { o->we = 0; o->cpu_slot = 0; o->cpu_frozen = 0; o->cpu_tier_known = 0; o->cpu_eff_we = 0; o->cpu_prefix_ns = PGWT_CPU_NS_UNKNOWN; o->cpu_credited_ns = 0; }\nstatic void open_stop_crediting(struct pgwt_summary_writer *w,|' \
    "$WRITER" > "$WORK/s5a.c"
awk '
  /^int pgwt_summary_push_event/ { inpe = 1 }
  inpe && !done && /open_stop_crediting\(w, o\);/ {
      sub(/open_stop_crediting\(w, o\);/, "open_stop_crediting_uncounted(o);")
      done = 1
  }
  { print }' "$WORK/s5a.c" > "$WORK/s5.c"
run_one "S5: a second, UNCOUNTED retire path used by the close site" \
        "$TEST" "$WORK/s5.c" "10a"

# S6 — counted at the NOTE instead of at the retirement: the
# one-numerator shape. Every accepted note would count, so a healthy
# credited gap reads unplaced == 9 and "accepted then retired unplaced"
# stops being distinguishable from "accepted".
sed -e 's/^        w->open_cpu_unplaced_total++;/        (void)0;/' \
    -e 's/^        o->cpu_prefix_ns = cpu->cpu_prefix_ns;/        o->cpu_prefix_ns = cpu->cpu_prefix_ns;\n        w->open_cpu_unplaced_total++;/' \
    "$WRITER" > "$WORK/s6.c"
run_one "S6: counted at the note (both sides from the same event)" \
        "$TEST" "$WORK/s6.c" "10b"

# S7 — the section's dispatcher line removed from main(). #686 measured that
# this leaves the suite at exit 0. §10 therefore prints its OWN banner from
# inside the function, so the banner's ABSENCE is a witness; the check count
# is the second.
sed 's/^    test_attribution_counters();.*$/    \/* removed *\//' "$TEST" \
    > "$WORK/s7_test.c"
echo ""
echo "=== MUTANT: S7: the §10 dispatcher line removed from main()"
echo "    must turn red: the banner disappears and the check count drops"
build "$WORK/s7_test.c" "$WRITER" "$WORK/bin7"
"$WORK/bin7" > "$WORK/out7.txt" 2>&1
echo "  exit=$?  (#686: a missing section does NOT change the exit status)"
echo "  '-- 10:' banner lines: $(/usr/bin/grep -cE '^-- 10:' "$WORK/out7.txt") (baseline: $(/usr/bin/grep -cE '^-- 10:' "$WORK/base.txt"))"
/usr/bin/grep -E 'checks,' "$WORK/out7.txt" | sed 's/^/  mutant:   /'
/usr/bin/grep -E 'checks,' "$WORK/base.txt" | sed 's/^/  baseline: /'
