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
# The existing guard cases use throwaway trees via its `[repo-dir]` argument.
# Independent root-Makefile checks below also read this repository's real
# Makefile and compile an invented undeclared-function probe with its CFLAGS.
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

# check_makefile_cflags <makefile> <flags-variable>
# Independent of GUARD, which only checks tests/Makefile. CANNOT-SEE inputs
# refuse: missing files, ambiguous/absent definitions, or no $(CC) recipes.
# Join continuations before inspecting definitions and recipes; comments and
# BPF_CFLAGS must not satisfy either the flag or the $(CFLAGS) requirement.
# Compiler forwarding is not compilation. Sub-make CFLAGS overrides must
# preserve the flags too: command-line assignments replace the child defaults.
check_makefile_cflags() {
    local mk="$1" flags_var="$2"
    if [[ ! -f "$mk" || ! -r "$mk" ]]; then
        echo "REFUSE: cannot read Makefile $mk" >&2
        return 1
    fi
    if [[ ! "$flags_var" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]]; then
        echo "REFUSE: invalid CFLAGS variable name: $flags_var" >&2
        return 1
    fi
    if ! command -v awk >/dev/null 2>&1; then
        echo "REFUSE: awk not found — cannot inspect $mk" >&2
        return 1
    fi
    awk -v flags_var="$flags_var" -v flag='-Werror=implicit-function-declaration' '
        function reject(message) {
            print message > "/dev/stderr"
            failed = 1
        }
        function has_compile_command(command, rest, before) {
            rest = command
            while (match(rest, /[$][(]CC[)]/)) {
                before = substr(rest, 1, RSTART - 1)
                # Exclude only CC assignment values (bare or quoted).
                # Keep scanning: the same fragment may also compile.
                if (before !~ /(^|[[:space:]])CC="?$/ &&
                    before !~ ("(^|[[:space:]])CC=" sprintf("%c", 39) "$"))
                    return 1
                rest = substr(rest, RSTART + RLENGTH)
            }
            return 0
        }
        function inspect(line, value, commands, n, i, command, quote, end_quote) {
            sub(/[[:space:]]*#.*/, "", line)
            if (line !~ /^\t/ && line ~ assignment) {
                definitions++
                value = line
                sub(assignment, "", value)
                if (value ~ "(^|[[:space:]])" flag "([[:space:]]|$)")
                    has_flag = 1
            }
            if (line ~ /^\t/) {
                # Scan anywhere, including shell keywords and pipelines.
                # CC="$(CC)" on a sub-make is merely a forwarded value.
                sub(/^\t[[:space:]@+-]*/, "", line)
                n = split(line, commands, /&&|;/)
                for (i = 1; i <= n; i++) {
                    command = commands[i]
                    sub(/^[[:space:]]*/, "", command)
                    if (has_compile_command(command)) {
                        recipes++
                        if (!index(command, "$(" flags_var ")"))
                            reject("FAIL: " FILENAME ":" first_line \
                                   " $(CC) recipe does not pass $(" flags_var "): " line)
                    }
                    if (command ~ /^[$][(]MAKE[)]([[:space:]]|$)/ &&
                        command ~ "[[:space:]]" flags_var "=") {
                        overrides++
                        value = command
                        sub("^.*[[:space:]]" flags_var "=", "", value)
                        # Inspect the assigned value, not unrelated arguments
                        # that might also happen to mention $(CFLAGS).
                        quote = substr(value, 1, 1)
                        if (quote == "\"" || quote == sprintf("%c", 39)) {
                            value = substr(value, 2)
                            end_quote = index(value, quote)
                            if (!end_quote) value = ""
                            else value = substr(value, 1, end_quote - 1)
                        } else {
                            sub(/[[:space:]].*/, "", value)
                        }
                        if (!index(value, "$(" flags_var ")"))
                            reject("FAIL: " FILENAME ":" first_line \
                                   " $(MAKE) override does not preserve $(" flags_var "): " line)
                    }
                }
            }
        }
        BEGIN { assignment = "^[ ]*" flags_var "[[:space:]]*[:?+!]*=" }
        {
            if (pending == "") first_line = NR
            if (sub(/\\$/, "")) {
                pending = pending $0 " "
            } else {
                inspect(pending $0)
                pending = ""
            }
        }
        END {
            if (pending != "")
                reject("REFUSE: " FILENAME " has an unfinished continuation")
            if (definitions != 1)
                reject("REFUSE: " FILENAME " expected exactly 1 " flags_var \
                       " definition; found " (definitions + 0))
            else if (!has_flag)
                reject("FAIL: " FILENAME " " flags_var " is missing " flag)
            if (!recipes)
                reject("REFUSE: " FILENAME " has no $(CC) compile recipes")
            if (failed) exit 1
            print "implicit-decl flag: " recipes " $(CC) recipe(s) checked, all pass $(" flags_var ")"
            print "implicit-decl flag: " (overrides + 0) " sub-make override(s) checked"
        }
    ' "$mk"
}

expect_root_fail() {  # <makefile> <substring> <what>
    local mk="$1" needle="$2" what="$3" result status
    result="$(check_makefile_cflags "$mk" CFLAGS 2>&1)"; status=$?
    if (( status != 0 )) && [[ "$result" == *"$needle"* ]]; then
        report true "$what (exit $status, matched '$needle')"
    else
        report false "$what — expected nonzero exit and '$needle'; got exit $status: $result"
    fi
}

expect_root_ok() {  # <makefile> <what>
    local mk="$1" what="$2" result status
    result="$(check_makefile_cflags "$mk" CFLAGS 2>&1)"; status=$?
    if (( status == 0 )); then
        report true "$what (exit 0)"
    else
        report false "$what — expected exit 0; got exit $status: $result"
    fi
}

echo
echo "== root Makefile: live CFLAGS gate (unconditional) =="
ROOT_MK="$SCRIPT_DIR/../Makefile"
out="$(check_makefile_cflags "$ROOT_MK" CFLAGS 2>&1)"; code=$?
if (( code == 0 )); then
    report true "live root Makefile: $out"
else
    report false "live root Makefile (exit $code): $out"
fi

echo
echo "== root Makefile: broken or absent awk must REFUSE =="
for rc_stub in 127 126; do
    stub="$tmpdir/stub_awk_$rc_stub"; mkdir -p "$stub"
    printf '#!/bin/sh\nexit %s\n' "$rc_stub" > "$stub/awk"
    chmod +x "$stub/awk"
    out="$(PATH="$stub:$PATH" check_makefile_cflags "$ROOT_MK" CFLAGS 2>&1)"; code=$?
    if (( code != 0 )); then
        report true "live root Makefile: awk dependency exiting $rc_stub refuses (exit $code)"
    else
        report false "live root Makefile: awk dependency exiting $rc_stub — expected a refusal; got exit $code: $out"
    fi
done

empty="$tmpdir/empty_awk_path"; mkdir -p "$empty"
out="$(PATH="$empty" check_makefile_cflags "$ROOT_MK" CFLAGS 2>&1)"; code=$?
if (( code != 0 )) && [[ "$out" == *"REFUSE: awk not found"* ]]; then
    report true "live root Makefile: awk absent from PATH refuses (exit $code)"
else
    report false "live root Makefile: awk absent from PATH — expected a refusal; got exit $code: $out"
fi

echo
echo "== root Makefile: compiler RED halves with the actual CFLAGS =="
cat > "$tmpdir/implicit-probe.c" <<'C'
int main(void) { return pgwt_test_undeclared_probe_512(); }
C
# Let make extract/expand CC and CFLAGS from the real Makefile, preserving
# continuations, variable references and shell quoting (no eval or hardcoded
# replacement CFLAGS). Only these phony targets run; src/ is not built here.
# Modern clang/GCC already error by default. Start BOTH runs in warning mode
# to model older GCC: the shipped CFLAGS must override that mode in run 1,
# while removing exactly the required token must leave run 2 non-fatal.
cat > "$tmpdir/implicit-probe.mk" <<'MK'
.PHONY: pgwt-probe-strict-512 pgwt-probe-warning-512
pgwt-probe-strict-512:
	$(CC) -Wno-error=implicit-function-declaration $(CFLAGS) -c "$(PGWT_PROBE_SRC)" -o /dev/null
pgwt-probe-warning-512:
	$(CC) -Wno-error=implicit-function-declaration $(filter-out -Werror=implicit-function-declaration,$(CFLAGS)) -c "$(PGWT_PROBE_SRC)" -o /dev/null
MK
for mode in strict warning; do
    out="$(make --no-print-directory -s -C "$SCRIPT_DIR/.." \
        -f "$ROOT_MK" -f "$tmpdir/implicit-probe.mk" \
        "PGWT_PROBE_SRC=$tmpdir/implicit-probe.c" "pgwt-probe-$mode-512" 2>&1)"; code=$?
    diagnostic=false
    if [[ "$out" == *pgwt_test_undeclared_probe_512* ||
          "$out" == *"implicit declaration"* || "$out" == *"implicitly declared"* ]]; then
        diagnostic=true
    fi
    if [[ "$mode" == strict ]] && (( code != 0 )) && [[ "$diagnostic" == true ]]; then
        report true "root CFLAGS reject the undeclared probe (exit $code, diagnostic matched)"
    elif [[ "$mode" == warning ]] && (( code == 0 )) && [[ "$diagnostic" == true ]]; then
        report true "same probe without the CFLAGS flag is only a warning (exit 0, diagnostic matched)"
    else
        report false "root CFLAGS probe ($mode) — unexpected exit $code or diagnostic: $out"
    fi
done

echo
echo "== root Makefile: static RED and CANNOT-SEE fixtures =="
# RED independent of any compiler: mutate only the real CFLAGS definition.
sed '/^CFLAGS[[:space:]]*=/s/-Werror=implicit-function-declaration//g' \
    "$ROOT_MK" > "$tmpdir/root-no-flag.mk"
expect_root_fail "$tmpdir/root-no-flag.mk" 'missing -Werror=implicit-function-declaration' \
    "root gate rejects a dropped flag without invoking a compiler (el8 case)"

printf 'CFLAGS = -Werror=implicit-function-declaration\nall:\n\t@true\n' \
    > "$tmpdir/root-no-recipes.mk"
expect_root_fail "$tmpdir/root-no-recipes.mk" 'no $(CC) compile recipes' \
    'root gate refuses a Makefile with no $(CC) recipes'
expect_root_fail "$tmpdir/root-absent.mk" 'REFUSE' 'root gate refuses a missing Makefile'

cp "$ROOT_MK" "$tmpdir/root-ambiguous.mk"
printf '\nCFLAGS = -O0\n' >> "$tmpdir/root-ambiguous.mk"
expect_root_fail "$tmpdir/root-ambiguous.mk" 'expected exactly 1' \
    'root gate refuses ambiguous CFLAGS definitions'
printf 'all:\n\t$(CC) $(CFLAGS) -c probe.c\n' > "$tmpdir/root-no-definition.mk"
expect_root_fail "$tmpdir/root-no-definition.mk" 'expected exactly 1' \
    'root gate refuses an absent CFLAGS definition'

cp "$ROOT_MK" "$tmpdir/root-bypass.mk"
printf '\nbypass:\n\t$(CC) $(BPF_CFLAGS) -c probe.c\n' >> "$tmpdir/root-bypass.mk"
expect_root_fail "$tmpdir/root-bypass.mk" 'does not pass $(CFLAGS)' \
    'root gate rejects a recipe passing only BPF_CFLAGS'

echo
echo "== root Makefile: command tokens and sub-make overrides =="
cat > "$tmpdir/root-forward.mk" <<'MK'
CFLAGS = -Wall -Werror=implicit-function-declaration
all:
	$(CC) $(CFLAGS) -c probe.c
	$(MAKE) -C tests CC="$(CC)" LIBBPF_LIB="../build/libbpf.a -lelf -lz"
MK
expect_root_ok "$tmpdir/root-forward.mk" \
    'root gate approves compiler forwarding without a CFLAGS override'

for recipe in 'cd x && $(CC) -o a a.c' '@-+$(CC) -o a a.c' 'cd x; $(CC) -o a a.c'; do
    cp "$tmpdir/root-forward.mk" "$tmpdir/root-command-bypass.mk"
    printf '\t%s\n' "$recipe" >> "$tmpdir/root-command-bypass.mk"
    expect_root_fail "$tmpdir/root-command-bypass.mk" 'does not pass $(CFLAGS)' \
        "root gate rejects compiler command without flags: $recipe"
done

# Each wrapped compile has a compliant sibling: missing it must fail for
# missing CFLAGS, never merely because the gate saw no compile recipes.
for recipe in 'for f in extra/*.c; do $(CC) -c $$f; done' \
              '@if true; then $(CC) -c probe.c; fi' \
              'echo probe.c | xargs $(CC) -c' \
              'false || $(CC) -c probe.c' \
              '($(CC) -c probe.c)' \
              'if false; then true; else $(CC) -c probe.c; fi' \
              'env CC="$(CC)" $(CC) -c probe.c'; do
    cat > "$tmpdir/root-wrapped-bypass.mk" <<'MK'
CFLAGS = -Wall -Werror=implicit-function-declaration
good:
	$(CC) $(CFLAGS) -c main.c
bad:
MK
    printf '\t%s\n' "$recipe" >> "$tmpdir/root-wrapped-bypass.mk"
    expect_root_fail "$tmpdir/root-wrapped-bypass.mk" 'does not pass $(CFLAGS)' \
        "root gate rejects wrapped compile with compliant sibling: $recipe"
done

cp "$tmpdir/root-forward.mk" "$tmpdir/root-override.mk"
printf '\t$(MAKE) -C tests CFLAGS="-O2"\n' >> "$tmpdir/root-override.mk"
expect_root_fail "$tmpdir/root-override.mk" 'override does not preserve $(CFLAGS)' \
    'root gate rejects a sub-make CFLAGS override with no compiler token'

cp "$tmpdir/root-forward.mk" "$tmpdir/root-override-unrelated.mk"
printf '\tcd x && $(MAKE) -C tests CFLAGS=-O2 OTHER="$(CFLAGS)"\n' \
    >> "$tmpdir/root-override-unrelated.mk"
expect_root_fail "$tmpdir/root-override-unrelated.mk" 'override does not preserve $(CFLAGS)' \
    'root gate rejects an override despite CFLAGS in an unrelated argument'

cp "$tmpdir/root-forward.mk" "$tmpdir/root-override-preserved.mk"
printf '\t@+$(MAKE) pgwt-server CFLAGS="$(CFLAGS) -fsanitize=address"\n' \
    >> "$tmpdir/root-override-preserved.mk"
expect_root_ok "$tmpdir/root-override-preserved.mk" \
    'root gate approves a sub-make override preserving CFLAGS (test-asan shape)'

printf 'CFLAGS = -Werror=implicit-function-declaration\nall:\n\t$(MAKE) -C tests CC="$(CC)"\n' \
    > "$tmpdir/root-forward-only.mk"
expect_root_fail "$tmpdir/root-forward-only.mk" 'no $(CC) compile recipes' \
    'root gate refuses forwarding alone as evidence of compilation'

# Exercise both arguments and continuation handling on a positive fixture.
printf 'USER_CFLAGS = -Wall \\\n -Werror=implicit-function-declaration\nall:\n\t$(CC) \\\n $(USER_CFLAGS) -c probe.c\n' \
    > "$tmpdir/root-custom.mk"
out="$(check_makefile_cflags "$tmpdir/root-custom.mk" USER_CFLAGS 2>&1)"; code=$?
if (( code == 0 )); then
    report true 'root gate supports another CFLAGS variable and continued recipes'
else
    report false "root gate with USER_CFLAGS and continuations (exit $code): $out"
fi

echo
echo "runner-pin guard self-test: $((tests_run - tests_failed))/$tests_run passed"
if (( tests_failed > 0 )); then
    echo "runner-pin guard self-test: FAILED"
    exit 1
fi
echo "runner-pin guard self-test: all tests passed"
exit 0
