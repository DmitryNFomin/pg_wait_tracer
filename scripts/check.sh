#!/usr/bin/env bash
# check.sh — the DETERMINISTIC test tier, runnable on a developer Mac with no
# Linux, no PostgreSQL, no root. This is what `make check` runs and what the
# push guard (scripts/hooks/push-guard.sh) requires before `git push`.
#
#   make check        full local tier (~4 min: adds the Playwright UI suite)
#   make check-fast   node + go + python compile only (seconds)
#
# Everything that needs the Linux build (C units, synthetic-data tests against
# pgwt-server, protocol drift, live capture) runs via `make box-check`.
#
# On success writes .pgwt-check.stamp = hash of the exact working tree that
# passed, so the guard can tell whether the tree changed since.
set -uo pipefail
cd "$(dirname "$0")/.."

FAST=0
[[ "${1:-}" == "--fast" ]] && FAST=1

fail=0
step() { printf '\n\033[1m=== %s ===\033[0m\n' "$1"; }
run()  { if ! "$@"; then echo "FAIL: $*"; fail=1; fi; }

need() { command -v "$1" >/dev/null || { echo "missing: $1 — see CLAUDE.md 'Local setup'"; exit 2; }; }
need node; need go; need python3

step "web builder unit tests (node)"
run node --test 'tests/web_unit/*.test.mjs'

step "go bridge (skips server-backed cases without a Linux pgwt-server)"
run bash -c 'cd web && go vet ./... && go test ./...'

step "python: compile every test module"
run python3 -m py_compile tests/*.py

if [[ $FAST -eq 0 ]]; then
    if ! python3 -c 'import playwright' 2>/dev/null; then
        echo "playwright missing — see CLAUDE.md 'Local setup'"; exit 2
    fi
    step "web UI suite vs mock_server.py (Playwright)"
    run python3 tests/test_web_ui.py
    step "web UI chaos suite (latency jitter / reconnects)"
    run python3 tests/test_web_ui_chaos.py
fi

if [[ $fail -ne 0 ]]; then
    echo; echo "CHECK FAILED"; rm -f .pgwt-check.stamp; exit 1
fi

# Stamp the exact tree (tracked + untracked, minus ignored) that passed.
scripts/tree-hash.sh > .pgwt-check.stamp
echo; echo "CHECK PASSED ($( [[ $FAST -eq 1 ]] && echo fast || echo full )) — stamp $(cat .pgwt-check.stamp)"
[[ $FAST -eq 1 ]] && echo "note: --fast skips the UI suite; the push guard accepts it, CI does not."
exit 0
