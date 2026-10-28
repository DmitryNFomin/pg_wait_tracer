#!/usr/bin/env bash
# mutation-red-suite.sh — every way §9 of tests/test_open_cpu_accounting.c
# (open_cpu_notes_total) could be made UNREACHABLE or VACUOUS, and the proof
# that each one turns it red.
#
# Showing a gate can go red proves the true-positive path, which has never
# been the broken one here. The defects that keep getting through are false
# NEGATIVES: a counter whose two sides come from the same place, a check
# satisfied without checking anything, a section that is not dispatched. So
# each mutant below is one of those, applied to src/summary_writer.c (or to
# the test's own main()), and the §9 line it must kill is named.
#
# Runs on macOS or Linux with no BPF and no PostgreSQL: the test is a server
# build. lz4 headers are taken from `brew --prefix lz4` when present.
#
# Usage: tests/results/issue424-notes-counter/mutation-red-suite.sh
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/v9mutXXXXXX")"
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
    sed -n '/-- 9:/,$p' "$WORK/out.txt" | /usr/bin/grep -E 'FAIL:|checks,' \
        | sed 's/^/  /'
    echo "  exit=$rc"
}

echo "### BASELINE (unmutated) — all of §9 must be green ###"
build "$TEST" "$WRITER" "$WORK/base" || { echo "baseline build failed"; exit 1; }
"$WORK/base" > "$WORK/base.txt" 2>&1
echo "  exit=$?"
sed -n '/-- 9:/,$p' "$WORK/base.txt" | /usr/bin/grep -E 'ok:|FAIL:|checks,' \
    | sed 's/^/  /'

# M1 — the increment moved ABOVE the on-CPU class test, so every note counts.
# "Never asked" then reads identical to "asked", and the fixture would FAIL
# every healthy wait-only capture while blaming the credit path.
sed 's/    if (on_cpu_note && cpu)/    if (1)/' "$WRITER" > "$WORK/m1.c"
run_one "M1: counted for EVERY note (class test bypassed)" \
        "$TEST" "$WORK/m1.c" "9c, 9d"

# M2 — the `cpu` test dropped. The v8 no-observation path (correct behaviour
# on a sampled/lightweight tier) then reads as asked.
sed 's/    if (on_cpu_note && cpu)/    if (on_cpu_note)/' "$WRITER" > "$WORK/m2.c"
run_one "M2: the observation-carrying test dropped" \
        "$TEST" "$WORK/m2.c" "9d"

# M3 — the increment deleted. The counter is 0 forever, so the live fixture
# REFUSES on every run and its assertion becomes permanently unreachable:
# a gate that cannot see. It must refuse, and 9a is what notices.
sed 's/^        w->open_cpu_notes_total++;/        (void)0;/' \
    "$WRITER" > "$WORK/m3.c"
run_one "M3: increment deleted (counter permanently 0)" \
        "$TEST" "$WORK/m3.c" "9a, 9b"

# M4 — counted where credit is PLACED instead of where the note arrives.
# This is the conservation-check-with-one-numerator shape: both sides of the
# fixture's reading would come from the same event, so asked-and-dropped
# becomes invisible, which is the exact state being diagnosed.
sed -e 's/^        w->open_cpu_notes_total++;/        (void)0;/' \
    -e 's/^            w->open_cpu_credited_ns_total += slice;/            w->open_cpu_credited_ns_total += slice; w->open_cpu_notes_total++;/' \
    "$WRITER" > "$WORK/m4.c"
run_one "M4: counted at the CREDIT instead of at the note" \
        "$TEST" "$WORK/m4.c" "9b"

# M5 — counted after the io_worker and table-full refusals, i.e. only for
# notes that survive them. Those refusals are precisely the silent early
# exits the counter exists to expose.
sed -e 's/^        w->open_cpu_notes_total++;/        (void)0;/' \
    -e 's/^    o->cat_flag = cat_flag;/    o->cat_flag = cat_flag;\n    if (on_cpu_note \&\& cpu) w->open_cpu_notes_total++;/' \
    "$WRITER" > "$WORK/m5.c"
run_one "M5: counted after the refusals it exists to expose" \
        "$TEST" "$WORK/m5.c" "9b"

# M6 — the section's dispatcher line removed from main(). #686 measured that
# this leaves the suite at exit 0. §9 therefore prints its OWN banner from
# inside the function, so the banner's ABSENCE is a witness; the check count
# is the second.
sed 's/^    test_notes_counter();.*$/    \/* removed *\//' "$TEST" > "$WORK/m6_test.c"
echo ""
echo "=== MUTANT: M6: the §9 dispatcher line removed from main()"
echo "    must turn red: the banner disappears and the check count drops"
build "$WORK/m6_test.c" "$WRITER" "$WORK/bin6"
"$WORK/bin6" > "$WORK/out6.txt" 2>&1
echo "  exit=$?  (#686: a missing section does NOT change the exit status)"
echo "  '-- 9:' banner lines: $(/usr/bin/grep -cE '^-- 9:' "$WORK/out6.txt") (baseline: $(/usr/bin/grep -cE '^-- 9:' "$WORK/base.txt"))"
/usr/bin/grep -E 'checks,' "$WORK/out6.txt" | sed 's/^/  mutant:   /'
/usr/bin/grep -E 'checks,' "$WORK/base.txt" | sed 's/^/  baseline: /'
