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
# TWO PHASES, not one flat pool — found the hard way (review round on the
# original single-pool design): `ui_main`'s own
# test_legend_hover_survives_tick has a 5s wait tied to a live-tick rebuild,
# and sharing a CPU with even ONE other headless Chromium sometimes pushes it
# past that window. Measured, not assumed: standalone, `--group main` passed
# 349/349 across repeated runs; in the original flat pool at
# PGWT_CHECK_PARALLEL_CAP=7 a reviewer got 1 failure in 3 runs, and dropping
# to CAP=2 (still sharing a slot with one sibling job) still failed 1 run in
# 4. CAP=1 (no sharing at all) was clean, but that is the serial runtime with
# extra bookkeeping — no win. So `ui_main` now runs ALONE, in its own phase,
# never sharing a CPU with another suite; every other job (short-lived, no
# reported contention sensitivity) still runs together beforehand:
#
#   Phase 1 (concurrent, PGWT_CHECK_PARALLEL_CAP, default 6 = uncapped):
#     node, go, ui_b5, ui_compare, ui_reconnect, ui_chaos
#   Phase 2 (always solo): ui_main
#
# 3 of Phase 1's 6 jobs launch a headless Chromium; CLAUDE.md warns this Mac
# is memory-constrained and has OOM-killed long runs before, so peak memory
# was measured, not assumed — see the CAP comment below.
#
# Ordering WITHIN a job's own test list is untouched: this only parallelizes
# whole, already-isolated suites/groups against each other. The aggregation
# core (semaphore + grouped output + crash detection) lives in
# scripts/lib/parallel_runner.sh so tests/test_check_parallel.sh can
# regression-test it directly against synthetic jobs; both phases below call
# it, so a failure in either phase is reported with the same
# SUITE FAILED/CRASHED attribution and neither phase can mask the other's
# exit status.
set -uo pipefail
cd "$(dirname "$0")/.."
source scripts/lib/parallel_runner.sh

# ── Phase 1: everything except ui_main, concurrent. ─────────────────────────
# Default cap is 6 = uncapped (all of Phase 1 at once): measured, not
# assumed — a full `make check` run (3 of Phase 1's 6 jobs launch a headless
# Chromium) peaked at 2.29GB RSS summed over the whole check.sh process tree
# on this 32GB Mac, sampled once/second — comfortable, and lower than the
# original 5-browser single-pool design's 2.87-2.96GB, since Phase 2 (ui_main)
# never overlaps with Phase 1 at all. Override to a lower number (e.g.
# PGWT_CHECK_PARALLEL_CAP=3) on a smaller machine.
CAP="${PGWT_CHECK_PARALLEL_CAP:-6}"
RESULTS_DIR="tests/results/check-parallel/phase1"

# name -> command, kept as parallel arrays (portable to bash 3.2, which is
# still /bin/bash's default on macOS — no associative arrays).
JOB_NAMES=(node go ui_b5 ui_compare ui_reconnect ui_chaos)
JOB_CMDS=(
    "node --test 'tests/web_unit/*.test.mjs'"
    "cd web && go vet ./... && go test ./..."
    "python3 tests/test_web_ui.py --group b5"
    "python3 tests/test_web_ui.py --group compare"
    "python3 tests/test_web_ui.py --group reconnect"
    "python3 tests/test_web_ui_chaos.py"
)

echo
printf '\033[1m=== check-parallel phase 1/2: node, go, ui_b5, ui_compare, ui_reconnect, ui_chaos (concurrent, cap=%s) ===\033[0m\n' "$CAP"
run_parallel_jobs
phase1_rc=$?

# ── Phase 2: ui_main, alone — see the header comment for why. ──────────────
JOB_NAMES=(ui_main)
JOB_CMDS=("python3 tests/test_web_ui.py --group main")
CAP=1
RESULTS_DIR="tests/results/check-parallel/phase2"

printf '\033[1m=== check-parallel phase 2/2: ui_main (solo, no concurrent suite) ===\033[0m\n'
run_parallel_jobs
phase2_rc=$?

if [[ $phase1_rc -ne 0 || $phase2_rc -ne 0 ]]; then
    exit 1
fi
exit 0
