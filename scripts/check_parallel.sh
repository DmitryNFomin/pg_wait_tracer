#!/usr/bin/env bash
# check_parallel.sh — run the independent-and-slow part of `make check`
# (the four isolated test_web_ui.py groups, the chaos suite, the Node
# builder tests, and the Go bridge tests) CONCURRENTLY instead of one after
# another, with each suite's output buffered and printed grouped so a
# failure stays attributable (never interleaved).
#
# Called by scripts/check.sh with the port env vars (PGWT_TEST_PORT,
# PGWT_CHAOS_PORT) already exported from ONE free_ports.py allocation, so
# every job's ports are fixed, non-overlapping offsets — no lock, no
# contention, see the PORT MAP comment in check.sh.
#
# Concurrency is capped (PGWT_CHECK_PARALLEL_CAP, default 7 = uncapped: all
# jobs at once). 5 of the 7 jobs launch a headless Chromium; CLAUDE.md warns
# this Mac is memory-constrained and has OOM-killed long runs before, so this
# was measured, not assumed: all 7 jobs running at once peaked at ~2.6GB RSS
# summed across every chromium/node/python3/go process on this machine (32GB
# total) — comfortable, so the default is uncapped. Override to a lower
# number (e.g. PGWT_CHECK_PARALLEL_CAP=4) on a smaller machine.
#
# Ordering WITHIN a job's own test list is untouched: this only parallelizes
# whole, already-isolated suites/groups against each other. The aggregation
# core (semaphore + grouped output + crash detection) lives in
# scripts/lib/parallel_runner.sh so tests/test_check_parallel.sh can
# regression-test it directly against synthetic jobs.
set -uo pipefail
cd "$(dirname "$0")/.."
source scripts/lib/parallel_runner.sh

CAP="${PGWT_CHECK_PARALLEL_CAP:-7}"
RESULTS_DIR="tests/results/check-parallel"

# name -> command, kept as parallel arrays (portable to bash 3.2, which is
# still /bin/bash's default on macOS — no associative arrays).
JOB_NAMES=(node go ui_main ui_b5 ui_compare ui_reconnect ui_chaos)
JOB_CMDS=(
    "node --test 'tests/web_unit/*.test.mjs'"
    "cd web && go vet ./... && go test ./..."
    "python3 tests/test_web_ui.py --group main"
    "python3 tests/test_web_ui.py --group b5"
    "python3 tests/test_web_ui.py --group compare"
    "python3 tests/test_web_ui.py --group reconnect"
    "python3 tests/test_web_ui_chaos.py"
)

run_parallel_jobs
