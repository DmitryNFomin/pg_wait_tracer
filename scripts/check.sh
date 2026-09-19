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

# ── Port allocation (so two `make check` runs on this Mac don't collide) ────
# One free base per run, laid out at FIXED OFFSETS below so every mock server
# the run spawns gets a run-private port; no lock (that would serialize two
# ~4-minute runs), see tests/free_ports.py. A developer running one suite by
# hand (no env vars set) keeps today's fixed-default ports.
#
#   PORT MAP (relative to PGWT_PORT_BASE):
#     PGWT_TEST_PORT  = base+0   test_web_ui.py:      HTTP +0/WS +1, B5 +10/+11, compare +20/+21
#     PGWT_CHAOS_PORT = base+30  test_web_ui_chaos.py: HTTP +30/WS +31
#     PGWT_SNAP_PORT  = base+40  test_web_ui_snapshots.py (not run here, exported
#                                for a follow-up manual run): HTTP +40/WS +41, sampled +50/+51
PORT_SPAN=60
PGWT_PORT_BASE=$(python3 tests/free_ports.py "$PORT_SPAN") || { echo "free_ports: could not allocate $PORT_SPAN free ports"; exit 2; }
export PGWT_TEST_PORT=$PGWT_PORT_BASE
export PGWT_CHAOS_PORT=$((PGWT_PORT_BASE + 30))
export PGWT_SNAP_PORT=$((PGWT_PORT_BASE + 40))
echo "port base: $PGWT_PORT_BASE (span $PORT_SPAN — TEST=+0 CHAOS=+30 SNAP=+40)"

step "web builder unit tests (node)"
run node --test 'tests/web_unit/*.test.mjs'

step "go bridge (skips server-backed cases without a Linux pgwt-server)"
run bash -c 'cd web && go vet ./... && go test ./...'

step "python: compile every test module"
run python3 -m py_compile tests/*.py

step "python: free_ports self-test"
run python3 tests/test_free_ports.py

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
