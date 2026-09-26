#!/usr/bin/env bash
# test_classify_changed_files.sh -- table-driven test for
# .github/scripts/classify_changed_files.sh (issue #167: the CI change
# classifier had no test, so editing it could only be validated by pushing
# and burning gate-box queue time).
#
# Pure bash, no git, no network, no Linux-only dependency -- runs on the Mac
# via `make check` (scripts/check.sh), not just on the box.
#
# Usage: tests/test_classify_changed_files.sh (run from anywhere)
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CLASSIFY="$SCRIPT_DIR/../.github/scripts/classify_changed_files.sh"

tests_run=0
tests_passed=0
tests_failed=0

report() {
    local ok="$1" msg="$2"
    tests_run=$((tests_run + 1))
    if [[ "$ok" == "true" ]]; then
        tests_passed=$((tests_passed + 1))
        echo "  PASS: $msg"
    else
        tests_failed=$((tests_failed + 1))
        echo "  FAIL: $msg"
    fi
}

# run_case NAME EVENT FILES EXPECT_SRC EXPECT_DOCS
# FILES: newline-separated (use $'a\nb'), or "" for empty stdin.
run_case() {
    local name="$1" event="$2" files="$3" expect_src="$4" expect_docs="$5"
    local out src docs ok
    out="$(printf '%s' "$files" | "$CLASSIFY" "$event")"
    src="$(grep '^src_changed=' <<<"$out" | cut -d= -f2)"
    docs="$(grep '^docs_only=' <<<"$out" | cut -d= -f2)"

    if [[ "$src" == "$expect_src" ]]; then ok=true; else ok=false; fi
    report "$ok" "$name: src_changed (got '$src', want '$expect_src')"

    if [[ "$docs" == "$expect_docs" ]]; then ok=true; else ok=false; fi
    report "$ok" "$name: docs_only (got '$docs', want '$expect_docs')"
}

# 1. only docs/
run_case "only docs/" "pull_request" $'docs/ROADMAP_AND_STATUS.md\ndocs/notes.txt' \
    false true

# 2. only *.md (outside docs/)
run_case "only *.md outside docs/" "pull_request" $'README.md\nCHANGES.md' \
    false true

# 3. only src/
run_case "only src/" "pull_request" $'src/server.c\nsrc/bpf/tracer.bpf.c' \
    true false

# 4. only web/
run_case "only web/" "pull_request" $'web/static/views/waterfall.js\nweb/bridge.go' \
    false false

# 5. only tests/
run_case "only tests/" "pull_request" $'tests/test_web_ui.py\ntests/run_all.sh' \
    false false

# 6. a mix of src/ and docs
run_case "mix of src/ and docs" "pull_request" $'src/server.c\ndocs/ROADMAP_AND_STATUS.md' \
    true false

# 7. a rename (git diff --name-only reports only the new path, one line)
run_case "a rename (tests/old.py -> tests/new.py)" "pull_request" \
    "tests/new_test.py" \
    false false

# 8. an empty file list (e.g. push with an unresolvable `before` SHA) --
# "no files detected" is treated as "run everything", never as docs-only.
run_case "empty file list" "push" "" \
    true false

# 9. a merge_group shaped input -- full matrix regardless of any files,
# since a queue entry can bundle several PRs and there is no single stable
# diff for that batch (see the script's own comment).
run_case "merge_group event (files present but must be ignored)" "merge_group" \
    $'docs/ROADMAP_AND_STATUS.md\nREADME.md' \
    true false
run_case "merge_group event (no files)" "merge_group" "" \
    true false

echo
echo "classify_changed_files: $tests_passed/$tests_run passed"
[[ $tests_failed -eq 0 ]]
