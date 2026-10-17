#!/usr/bin/env bash
# check-runner-registration.sh -- reconcile tests/run_all.sh (make
# box-check) against tests/ci_smoke.sh (CI/nightly) membership (issue #433).
# See scripts/check-runner-registration.py's docstring for the full
# rationale and the three real incidents this closes. This is a
# reviewer-run guard like scripts/check-test-registration.sh -- NOT wired
# into `make check`, CI, or any Makefile target; a reviewer runs it by hand
# before declaring READY. Self-tests: tests/test_check_runner_registration.sh.
#
# A guard that cannot run must refuse, not silently fall through (issue
# #431's defect): a missing python3 is a loud, immediate, fail-closed error.
#
# Usage: scripts/check-runner-registration.sh [repo-dir]
set -uo pipefail

if ! command -v python3 >/dev/null 2>&1; then
    echo "check-runner-registration: missing required tool 'python3' -- refusing" \
         "to approve blind." >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
exec python3 "$SCRIPT_DIR/check-runner-registration.py" "$@"
