#!/usr/bin/env bash
# check-section-registration.sh -- verify section owners are reachable from
# main in every sectioned C test suite (#686). See the Python docstring for
# rationale, measured losses, scope and limits. Self-tests:
# tests/test_check_section_registration.sh. Both run in scripts/check.sh.
#
# Usage: scripts/check-section-registration.sh [repo-dir]
set -uo pipefail

if ! command -v python3 >/dev/null 2>&1; then
    echo "REFUSING: check-section-registration: missing required tool 'python3' --" \
         "refusing to approve blind." >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)" || exit 1
# Do not exec: a broken interpreter/shim must be distinguished from a verdict.
python3 "$SCRIPT_DIR/check-section-registration.py" "$@"
status=$?
if [[ $status -eq 126 || $status -eq 127 ]]; then
    echo "REFUSING: check-section-registration: could not run the checker" \
         "(python3 exited $status)." >&2
    exit 1
fi
exit "$status"
