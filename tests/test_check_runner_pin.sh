#!/usr/bin/env bash
# test_check_runner_pin.sh — unit + BYPASS suite for scripts/check-runner-pin.sh
# (the runner-image pin and the -Werror=implicit-function-declaration flag,
# 2026-10-22 ubuntu-latest -> 26.04 image roll).
#
# Two halves, and the second is the point:
#   TRUE POSITIVE  — the guard goes red on each shape it exists to catch
#                    (a floating label anywhere, in any file, in any job, in a
#                    ternary fallback, behind a reassuring comment; a CFLAGS
#                    that lost the flag; a recipe that stopped passing CFLAGS).
#   CANNOT-SEE     — every way the guard could be made BLIND must REFUSE, not
#                    approve: no workflow dir, no workflow files, a workflow
#                    with no runs-on at all, a runs-on it cannot resolve (block
#                    form / matrix indirection), no tests/Makefile, ambiguous
#                    CFLAGS, no compile recipes, and a `grep` dependency that
#                    exits 126/127 or is missing from PATH entirely.
#
# No network, no git, does not read this repository's own state: every case
# builds a throwaway tree under `mktemp -d` and points the guard at it via its
# `[repo-dir]` argument.
#
# Usage: tests/test_check_runner_pin.sh      (wired into `make check`)
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
GUARD="$SCRIPT_DIR/../scripts/check-runner-pin.sh"
# Absolute bash: the PATH-stripping cases below must still be able to LAUNCH
# the guard, otherwise they would "pass" on env(1) failing to find bash rather
# than on the guard refusing.
BASH_BIN="$(command -v bash)"

tests_run=0; tests_failed=0
report() {
    local ok="$1" msg="$2"
    tests_run=$((tests_run + 1))
    if [[ "$ok" == "true" ]]; then
        echo "  PASS: $msg"
    else
        tests_failed=$((tests_failed + 1))
        echo "  FAIL: $msg"
    fi
}

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

# ── fixtures ────────────────────────────────────────────────────────────────

# write_good <repo> — a minimal tree the guard must APPROVE.
write_good() {
    local repo="$1"
    mkdir -p "$repo/.github/workflows" "$repo/tests"
    cat > "$repo/.github/workflows/ci.yml" <<'YML'
name: CI
jobs:
  one:
    runs-on: ubuntu-24.04
  two:
    runs-on: ${{ github.event_name == 'pull_request' && 'ubuntu-24.04' || fromJSON('["self-hosted","gate-box"]') }}
YML
    write_makefile "$repo" '-g -O2 -Wall -Werror=implicit-function-declaration -I../src'
}

# write_makefile <repo> <cflags-value>
write_makefile() {
    local repo="$1" cflags="$2"
    mkdir -p "$repo/tests"
    {
        printf 'CC       ?= gcc\n'
        printf 'CFLAGS    = %s\n' "$cflags"
        printf 'test_a: test_a.c\n\t$(CC) $(CFLAGS) -o $@ $^\n'
        printf 'test_b: test_b.c\n\t$(CC) $(CFLAGS) -o $@ $^ -lm\n'
    } > "$repo/tests/Makefile"
}

# run_guard <repo> [env-prefix...] — captures combined output + exit code.
out=""; code=0
run_guard() {
    local repo="$1"; shift
    out="$("$@" "$BASH_BIN" "$GUARD" "$repo" 2>&1)"; code=$?
}

expect_fail() {   # <repo> <substring> <what>
    local repo="$1" needle="$2" what="$3"
    run_guard "$repo" env
    if (( code != 0 )) && [[ "$out" == *"$needle"* ]]; then
        report true "$what (exit $code, matched '$needle')"
    else
        report false "$what — expected nonzero exit and '$needle'; got exit $code: $out"
    fi
}

expect_ok() {     # <repo> <what>
    local repo="$1" what="$2"
    run_guard "$repo" env
    if (( code == 0 )); then
        report true "$what (exit 0)"
    else
        report false "$what — expected exit 0; got $code: $out"
    fi
}

echo "== baseline: a correct tree is approved =="
G="$tmpdir/good"; write_good "$G"
expect_ok "$G" "pinned workflows + flag + CFLAGS-passing recipes"

echo
echo "== TRUE POSITIVES: the guard goes red on what it exists to catch =="

# TP1: one floating job alongside a pinned one (must not pass because SOME
# job is pinned).
R="$tmpdir/tp1"; write_good "$R"
cat > "$R/.github/workflows/ci.yml" <<'YML'
jobs:
  pinned:
    runs-on: ubuntu-24.04
  floating:
    runs-on: ubuntu-latest
YML
expect_fail "$R" "FLOATING" "TP1 floating job next to a pinned job"

# TP2: the fork-PR fallback inside a runs-on expression.
R="$tmpdir/tp2"; write_good "$R"
cat > "$R/.github/workflows/ci.yml" <<'YML'
jobs:
  gate:
    runs-on: ${{ github.event_name == 'pull_request' && 'ubuntu-latest' || fromJSON('["self-hosted","gate-box"]') }}
YML
expect_fail "$R" "FLOATING" "TP2 floating label in a ternary fallback"

# TP3: a comment claiming the line is pinned cannot launder a floating value.
R="$tmpdir/tp3"; write_good "$R"
cat > "$R/.github/workflows/ci.yml" <<'YML'
jobs:
  one:
    runs-on: ubuntu-latest   # PINNED, honestly
YML
expect_fail "$R" "FLOATING" "TP3 floating value behind a reassuring comment"

# TP4: a floating label in the SECOND workflow file (the guard must scan all
# files, not stop at the first clean one).
R="$tmpdir/tp4"; write_good "$R"
cat > "$R/.github/workflows/nightly.yml" <<'YML'
jobs:
  matrix:
    runs-on: ubuntu-latest
YML
expect_fail "$R" "nightly.yml" "TP4 floating label in a second workflow file"

# TP5: a non-ubuntu floating image.
R="$tmpdir/tp5"; write_good "$R"
cat > "$R/.github/workflows/ci.yml" <<'YML'
jobs:
  one:
    runs-on: macos-latest
YML
expect_fail "$R" "FLOATING" "TP5 macos-latest is floating too"

# TP6: CFLAGS lost the implicit-declaration flag (the tests/gen_test_traces.c
# regression shape).
R="$tmpdir/tp6"; write_good "$R"
write_makefile "$R" '-g -O2 -Wall -I../src'
expect_fail "$R" "missing -Werror=implicit-function-declaration" \
    "TP6 CFLAGS without the implicit-declaration flag"

# TP7: the flag is still in CFLAGS, but one recipe no longer passes $(CFLAGS)
# — the flag is present and does nothing for that TU.
R="$tmpdir/tp7"; write_good "$R"
printf 'test_c: test_c.c\n\t$(CC) -o $@ $^\n' >> "$R/tests/Makefile"
expect_fail "$R" "do not pass" "TP7 a recipe that bypasses \$(CFLAGS)"

echo
echo "== CANNOT-SEE: every blind spot must REFUSE, never approve =="

# R1: no workflow directory at all.
R="$tmpdir/r1"; write_good "$R"; rm -rf "$R/.github"
expect_fail "$R" "REFUSE" "R1 no .github/workflows directory"

# R2: workflow directory present but empty (empty input).
R="$tmpdir/r2"; write_good "$R"; rm -f "$R"/.github/workflows/*
expect_fail "$R" "no workflow files" "R2 workflow directory with no workflow files"

# R3: a workflow file that declares no runs-on at all (the thing being checked
# is ABSENT rather than wrong).
R="$tmpdir/r3"; write_good "$R"
printf 'name: X\non: push\njobs: {}\n' > "$R/.github/workflows/ci.yml"
expect_fail "$R" "declares no 'runs-on:'" "R3 workflow with no runs-on"

# R4: block/list form — no inline value to resolve.
R="$tmpdir/r4"; write_good "$R"
cat > "$R/.github/workflows/ci.yml" <<'YML'
jobs:
  one:
    runs-on:
      - self-hosted
      - gate-box
YML
expect_fail "$R" "block/list form" "R4 runs-on in block form is unresolvable"

# R5: indirected through matrix (unparseable field / unresolvable base).
R="$tmpdir/r5"; write_good "$R"
cat > "$R/.github/workflows/ci.yml" <<'YML'
jobs:
  one:
    runs-on: ${{ matrix.os }}
YML
expect_fail "$R" "unresolvable context" "R5 runs-on indirected through matrix"

# R6: no tests/Makefile — the flag half cannot be checked.
R="$tmpdir/r6"; write_good "$R"; rm -f "$R/tests/Makefile"
expect_fail "$R" "REFUSE" "R6 missing tests/Makefile"

# R7: two CFLAGS definitions — which one is in force is unknowable.
R="$tmpdir/r7"; write_good "$R"
printf 'CFLAGS    = -O0\n' >> "$R/tests/Makefile"
expect_fail "$R" "expected exactly 1" "R7 ambiguous CFLAGS definition"

# R8: a Makefile with the flag but no compile recipes — nothing for it to
# apply to, so "the flag is present" proves nothing.
R="$tmpdir/r8"; write_good "$R"
printf 'CFLAGS    = -Wall -Werror=implicit-function-declaration\nall:\n\t@true\n' \
    > "$R/tests/Makefile"
expect_fail "$R" "no \$(CC) compile recipes" "R8 Makefile with no compile recipes"

# R9/R10: the grep dependency exits 127 / 126. A guard whose only instrument
# is broken must refuse, not report "no floating labels found".
for rc_stub in 127 126; do
    R="$tmpdir/r_grep_$rc_stub"; write_good "$R"
    stub="$tmpdir/stub_$rc_stub"; mkdir -p "$stub"
    printf '#!/bin/sh\nexit %s\n' "$rc_stub" > "$stub/grep"
    chmod +x "$stub/grep"
    run_guard "$R" env "PATH=$stub:$PATH"
    if (( code != 0 )) && [[ "$out" == *"grep exited"* ]]; then
        report true "R grep dependency exiting $rc_stub refuses (exit $code)"
    else
        report false "R grep dependency exiting $rc_stub — expected a refusal; got exit $code: $out"
    fi
done

# R11: grep missing from PATH entirely.
R="$tmpdir/r11"; write_good "$R"
empty="$tmpdir/emptypath"; mkdir -p "$empty"
run_guard "$R" env "PATH=$empty"
if (( code != 0 )) && [[ "$out" == *"not found in PATH"* ]]; then
    report true "R11 grep absent from PATH refuses (exit $code)"
else
    report false "R11 grep absent from PATH — expected a refusal; got exit $code: $out"
fi

echo
echo "== NOT a false positive: a comment may mention the floating label =="
R="$tmpdir/fp1"; write_good "$R"
cat > "$R/.github/workflows/ci.yml" <<'YML'
jobs:
  one:
    runs-on: ubuntu-24.04   # PINNED, not ubuntu-latest - see the pin header
YML
expect_ok "$R" "FP1 pinned value whose comment names ubuntu-latest"

echo
echo "runner-pin guard self-test: $((tests_run - tests_failed))/$tests_run passed"
if (( tests_failed > 0 )); then
    echo "runner-pin guard self-test: FAILED"
    exit 1
fi
exit 0
