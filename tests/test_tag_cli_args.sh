#!/usr/bin/env bash
# test_tag_cli_args.sh -- unit test for tag_cli_args() in
# tests/live_daemon_lib.sh, the shared helper that threads a counted demo
# rehearsal's --tag through scripts/demo-rehearsal.sh ->
# scripts/demo-rehearsal-remote-run.sh -> tests/demo_rehearsal.sh ->
# tests/demo_rehearsal.py --tag (docs/DEMO_REHEARSAL_CRITERIA.md "a counted
# attempt names the tag it ran against").
#
# Pure bash, no Linux/root/live-PostgreSQL dependency -- runs on the Mac
# via `make check` (scripts/check.sh), not just on the box.
#
# Usage: tests/test_tag_cli_args.sh (run from anywhere)
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck disable=SC1091
source "$SCRIPT_DIR/live_daemon_lib.sh"

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

# 1. A given tag produces exactly the two-element --tag flag pair.
mapfile -t got < <(tag_cli_args "v1.2.3")
if [[ "${#got[@]}" -eq 2 && "${got[0]}" == "--tag" && "${got[1]}" == "v1.2.3" ]]; then
    ok=true
else
    ok=false
fi
report "$ok" "a given tag -> (--tag, v1.2.3) (got: ${got[*]-<empty>})"

# 2. No tag (empty string, scripts/demo-rehearsal.sh's TAG="${TAG:-}"
# default) produces NO arguments at all -- the absent-tag case must not
# synthesize a flag, so demo_rehearsal.py's own `args.tag or
# _detect_git_tag()` sees an actually-absent --tag and can still reach the
# honest `tag: null` when auto-detect also fails.
mapfile -t got < <(tag_cli_args "")
if [[ "${#got[@]}" -eq 0 ]]; then ok=true; else ok=false; fi
report "$ok" "empty tag -> no CLI args (got: ${got[*]-<empty>}, count=${#got[@]})"

# 3. Unset (no argument at all) behaves the same as empty -- the caller
# sites always pass "$TAG" so this is the realistic "unset TAG env var"
# shape after default-expansion, but tag_cli_args must not crash or
# misbehave if ever called bare.
mapfile -t got < <(tag_cli_args)
if [[ "${#got[@]}" -eq 0 ]]; then ok=true; else ok=false; fi
report "$ok" "no argument -> no CLI args (got: ${got[*]-<empty>}, count=${#got[@]})"

# 4. A tag containing no spaces/newlines round-trips byte-for-byte (the
# realistic shape: `git describe --tags --exact-match` output or a
# hand-typed release tag like demo-2026-10-13).
mapfile -t got < <(tag_cli_args "demo-2026-10-13")
if [[ "${#got[@]}" -eq 2 && "${got[1]}" == "demo-2026-10-13" ]]; then
    ok=true
else
    ok=false
fi
report "$ok" "tag value round-trips unmodified (got: ${got[1]-<empty>})"

echo
echo "tag_cli_args: $tests_passed/$tests_run passed"
[[ $tests_failed -eq 0 ]]
