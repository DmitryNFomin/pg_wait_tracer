#!/usr/bin/env bash
# test_box_check_object_excludes.sh -- issue #567 bypass suite for
# scripts/box-check.sh's up-rsync exclude list (stale/foreign build objects
# must never travel to the Linux box) and the remote `make -C tests clean`
# guard (a stale object already ON the box must not survive either).
#
# Every assertion below is on the ACTUAL TRANSFER DECISION, reproduced
# hermetically with `rsync --dry-run --itemize-changes` against a local
# destination directory (no ssh, no box needed -- CLAUDE.md's own suggested
# technique for this). The exclude flags used are EXTRACTED from the real
# scripts/box-check.sh source text at run time, not retyped here: a test
# that only checked "the exclude list contains the string *.o" would pass
# even if that string were never wired into the actual rsync invocation
# (dead array, wrong line, typo'd glob) -- see the #567 issue text. Running
# the real up-rsync command (with its destination swapped for a local dir)
# closes that gap: what is asserted is what rsync would actually do.
#
# Registered in tests/unit_tests.list (box-check/CI tier, Linux) -- NOT
# wired into scripts/check.sh (off limits to this branch; see the #567
# implementer contract). Needs nothing Linux-specific (plain bash + rsync +
# make), so it also runs correctly by hand on this Mac; it is simply not
# part of `make check`'s own list right now.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$SCRIPT_DIR/.."
BOX_CHECK="$REPO_ROOT/scripts/box-check.sh"

tests_run=0; tests_passed=0; tests_failed=0
report() {
    local ok="$1" msg="$2"
    tests_run=$((tests_run + 1))
    if [[ "$ok" == "true" ]]; then
        tests_passed=$((tests_passed + 1)); echo "  PASS: $msg"
    else
        tests_failed=$((tests_failed + 1)); echo "  FAIL: $msg"
    fi
}

# Tools this suite itself needs. Missing one must fail the whole suite
# loudly -- NOT be silently treated as "0 assertions, 0 failures = pass".
for t in rsync awk sed grep make; do
    command -v "$t" >/dev/null 2>&1 || { echo "FAIL: required tool '$t' not found in PATH"; exit 1; }
done

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

# ---------------------------------------------------------------------------
# up_rsync_dry_run SCRIPT SRC DST -- extracts the UP-rsync invocation from
# SCRIPT (the one anchored "rsync -az --delete -e \"\$rsync_e\"" at the
# START of a line, through the line containing its "$target:$remote_dir/"
# destination -- this is what distinguishes it from the DOWN-rsync a few
# lines later that pulls tests/results/ui_live/ back FROM the box, which is
# a single-line `if rsync ...` invocation with no leading anchor and no
# exclude list), swaps the ssh transport and remote destination for local
# ones, and runs it for real with --dry-run --itemize-changes against
# SRC/DST. Prints rsync's itemize output on stdout.
#
# Returns 2 (not rsync's own exit code) if the up-rsync invocation cannot
# even be found in SCRIPT -- a parser that found nothing MUST refuse, never
# silently report "0 lines transferred" as if that were a real negative
# result for every caller to interpret as "the test passed".
up_rsync_dry_run() {
    local script="$1" src="$2" dst="$3"
    [[ -f "$script" ]] || { echo "up_rsync_dry_run: $script does not exist" >&2; return 2; }
    local junk_def object_def block
    junk_def=$(sed -n '/^junk_excludes=(/,/^)/p' "$script")
    object_def=$(sed -n '/^object_excludes=(/,/^)/p' "$script")
    block=$(awk -v src="$src" -v dst="$dst" '
        /^rsync -az --delete -e "\$rsync_e"/ { p = 1 }
        p {
            line = $0
            gsub(/-e "\$rsync_e" /, "", line)
            gsub(/\.\/ "\$target:\$remote_dir\/"/, "\"" src "/\" \"" dst "/\"", line)
            sub(/ \|\| exit 1$/, "", line)
            print line
        }
        p && /\$target:\$remote_dir\/"/ { exit }
    ' "$script")
    [[ -n "$block" ]] || { echo "up_rsync_dry_run: no up-rsync invocation found in $script" >&2; return 2; }
    # junk_def / object_def may legitimately be empty (an old-style fixture
    # with no such array at all) -- that is exactly the pre-#567 shape this
    # suite's RED case below exercises, so it is not itself a parse error.
    local eval_cmd
    eval_cmd="$junk_def"$'\n'"$object_def"$'\n'"$block"' --dry-run --itemize-changes'
    eval "$eval_cmd"
}

# seed_src DIR -- a representative tree: a stale build object sitting where
# scripts/check-link-closure.sh's Mac-side compile leaves one (#567's exact
# trigger), a tracked-shaped source file alongside it (positive control --
# the exclude must not be so broad it eats real content), a static/shared
# lib (the issue's "while there" extras), the existing /build exclude's own
# target, and the existing junk-exclude's own target.
seed_src() {
    local dir="$1"
    mkdir -p "$dir/tests" "$dir/build"
    : > "$dir/tests/map_reader_core.o"
    printf 'int main(void){return 0;}\n' > "$dir/tests/test_wait_event.c"
    : > "$dir/build/pg_wait_tracer.o"
    : > "$dir/libfoo.a"
    : > "$dir/libbar.so"
    : > "$dir/.DS_Store"
}

# =============================================================================
# 1. BASELINE (the real, fixed scripts/box-check.sh): the stale object, the
#    lib artifacts, and the pre-existing exclusions must not transfer; the
#    ordinary source file must.
# =============================================================================
src1="$tmpdir/src1"; dst1="$tmpdir/dst1"; mkdir -p "$dst1"
seed_src "$src1"
out1=$(up_rsync_dry_run "$BOX_CHECK" "$src1" "$dst1" 2>&1); rc1=$?
[[ $rc1 -eq 0 ]] && ok=true || ok=false
report "$ok" "real box-check.sh: up-rsync dry-run against a local dest exits 0 (rc=$rc1, output: $out1)"
echo "$out1" | grep -q 'map_reader_core.o' && ok=false || ok=true
report "$ok" "real box-check.sh: tests/map_reader_core.o (stale object, #567) is NOT transferred"
echo "$out1" | grep -q 'libfoo.a' && ok=false || ok=true
report "$ok" "real box-check.sh: libfoo.a is NOT transferred"
echo "$out1" | grep -q 'libbar.so' && ok=false || ok=true
report "$ok" "real box-check.sh: libbar.so is NOT transferred"
echo "$out1" | grep -q 'pg_wait_tracer.o' && ok=false || ok=true
report "$ok" "real box-check.sh: build/pg_wait_tracer.o is still excluded (baseline /build exclude, unaffected by this change)"
echo "$out1" | grep -q '\.DS_Store' && ok=false || ok=true
report "$ok" "real box-check.sh: .DS_Store is still excluded (baseline junk exclude, unaffected by this change)"
echo "$out1" | grep -q 'test_wait_event\.c' && ok=true || ok=false
report "$ok" "real box-check.sh: tests/test_wait_event.c (ordinary tracked-shaped source) IS still transferred -- the exclude is not overly broad"

# =============================================================================
# 2. RED: a fixture reproducing the PRE-#567 up-rsync (no object_excludes
#    array, no reference to it on the invocation line -- exactly
#    origin/master's scripts/box-check.sh before this fix). Demonstrates
#    this suite actually distinguishes fixed from broken: the stale object
#    (and the other artifacts) MUST transfer here, or this test would be
#    incapable of ever failing.
# =============================================================================
old_script="$tmpdir/box-check-old.sh"
sed '/^object_excludes=($/,/^)$/d; /"${object_excludes\[@\]}" \\/d' "$BOX_CHECK" > "$old_script"
if grep -q '^object_excludes=(' "$old_script" || grep -q 'object_excludes\[@\]' "$old_script"; then
    echo "FAIL: fixture setup did not actually remove the object_excludes array/usage"; exit 1
fi
src2="$tmpdir/src2"; dst2="$tmpdir/dst2"; mkdir -p "$dst2"
seed_src "$src2"
out2=$(up_rsync_dry_run "$old_script" "$src2" "$dst2" 2>&1); rc2=$?
[[ $rc2 -eq 0 ]] && ok=true || ok=false
report "$ok" "pre-#567 fixture: up-rsync dry-run itself still runs (rc=$rc2)"
echo "$out2" | grep -q 'map_reader_core.o' && ok=true || ok=false
report "$ok" "RED: pre-#567 fixture (no object_excludes) DOES transfer tests/map_reader_core.o -- reproduces #567"
echo "$out2" | grep -q 'libfoo.a' && ok=true || ok=false
report "$ok" "RED: pre-#567 fixture DOES transfer libfoo.a"

# =============================================================================
# 3. REFUSE, not silent pass: a script with no up-rsync invocation at all
#    (the thing being checked is ABSENT, not wrong) must make the extractor
#    refuse (rc=2), never be read as "nothing excluded, nothing to send".
# =============================================================================
empty_script="$tmpdir/empty.sh"
echo '# not a real box-check.sh' > "$empty_script"
up_rsync_dry_run "$empty_script" "$tmpdir/src3" "$tmpdir/dst3" >/dev/null 2>&1
rc3=$?
[[ $rc3 -eq 2 ]] && ok=true || ok=false
report "$ok" "a script with no up-rsync invocation makes the extractor refuse (rc=$rc3, want 2), not silently approve"

missing_script="$tmpdir/does-not-exist.sh"
up_rsync_dry_run "$missing_script" "$tmpdir/src4" "$tmpdir/dst4" >/dev/null 2>&1
rc4=$?
[[ $rc4 -eq 2 ]] && ok=true || ok=false
report "$ok" "a missing script file makes the extractor refuse (rc=$rc4, want 2), not silently approve"

# =============================================================================
# 4. The remote build: `make -C tests clean` must run BEFORE `make -C
#    tests`, so a stale object already sitting in a persistent remote_dir
#    (from a run before this fix, or carried over any other way) cannot
#    survive on mtime grounds -- rsync's -t preserves the Mac's mtime, which
#    is typically newer than a freshly-synced source file, so plain `make`
#    would consider the stale .o up to date and never rebuild it.
# =============================================================================
remote_line=$(grep 'nproc' "$BOX_CHECK")
[[ -n "$remote_line" ]] && ok=true || ok=false
report "$ok" "found the remote build command line in box-check.sh"
clean_pos=$(printf '%s' "$remote_line" | grep -bo 'make -C tests clean' | head -1 | cut -d: -f1)
build_pos=$(printf '%s' "$remote_line" | grep -bo 'make -C tests &&' | head -1 | cut -d: -f1)
[[ -n "$clean_pos" && -n "$build_pos" ]] && ok=true || ok=false
report "$ok" "remote command contains both 'make -C tests clean' and 'make -C tests &&' (clean_pos=${clean_pos:-<missing>} build_pos=${build_pos:-<missing>})"
if [[ -n "$clean_pos" && -n "$build_pos" ]]; then
    [[ "$clean_pos" -lt "$build_pos" ]] && ok=true || ok=false
else
    ok=false
fi
report "$ok" "'make -C tests clean' runs BEFORE 'make -C tests' in the remote command"

# RED: a pre-#567-style remote line (no clean step) must be recognized as
# missing the guard, not silently accepted as "ordering is fine".
old_remote_line=$(printf '%s' "$remote_line" | sed 's/make -C tests clean && //')
old_clean_pos=$(printf '%s' "$old_remote_line" | grep -bo 'make -C tests clean' | head -1 | cut -d: -f1)
[[ -z "$old_clean_pos" ]] && ok=true || ok=false
report "$ok" "RED: pre-#567-style remote line (clean step removed) is correctly reported as missing 'make -C tests clean'"

# =============================================================================
# 5. Behavioral proof that `make -C tests clean` (the REAL tests/Makefile
#    rule, not a reimplementation) actually removes the stale-object class
#    at the heart of #567, regardless of mtime -- a freshly `touch`ed file
#    (newest possible mtime) is exactly the hardest case for a mtime-based
#    rebuild to catch, and `clean` doesn't look at mtimes at all.
# =============================================================================
clean_scratch="$tmpdir/clean_scratch"
mkdir -p "$clean_scratch"
cp "$REPO_ROOT/tests/Makefile" "$clean_scratch/"
: > "$clean_scratch/map_reader_core.o"
: > "$clean_scratch/test_wait_event"
: > "$clean_scratch/test_exec_index"
touch "$clean_scratch/map_reader_core.o"  # newest possible mtime on purpose
make -C "$clean_scratch" clean >/dev/null 2>&1
rc5=$?
[[ $rc5 -eq 0 ]] && ok=true || ok=false
report "$ok" "make -C tests clean (real tests/Makefile rule) exits 0 against a scratch copy (rc=$rc5)"
[[ ! -e "$clean_scratch/map_reader_core.o" ]] && ok=true || ok=false
report "$ok" "make -C tests clean removes a freshly-touched (newest-mtime) map_reader_core.o"
[[ ! -e "$clean_scratch/test_wait_event" && ! -e "$clean_scratch/test_exec_index" ]] && ok=true || ok=false
report "$ok" "make -C tests clean also removes stale test binaries (test_wait_event, test_exec_index)"

echo
echo "$tests_passed/$tests_run passed"
[[ $tests_failed -eq 0 ]]
