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
cd "$(dirname "$0")/.." || exit 2

# Running a check should install the push guard for this clone. Respect any
# hooks directory chosen by the developer, and never fail a check over config.
activate_push_guard() {
    local hooks_path status
    if hooks_path=$(git config --get core.hooksPath 2>/dev/null); then
        if [[ "$hooks_path" != "scripts/git-hooks" ]]; then
            printf "warning: core.hooksPath is '%s'; push guard is not active for this clone.\n" "$hooks_path" >&2
        fi
        return
    else
        status=$?
    fi

    if [[ $status -ne 1 ]] || ! git config --list >/dev/null 2>&1; then
        echo "warning: could not read Git config; push guard may not be active for this clone." >&2
    elif git config --local core.hooksPath scripts/git-hooks >/dev/null 2>&1; then
        echo "activated push guard: core.hooksPath=scripts/git-hooks (.git/hooks is bypassed)"
    else
        echo "warning: could not set core.hooksPath; push guard is not active for this clone." >&2
    fi
}
activate_push_guard

FAST=0
[[ "${1:-}" == "--fast" ]] && FAST=1

fail=0
step() { printf '\n\033[1m=== %s ===\033[0m\n' "$1"; }
run()  { if ! "$@"; then echo "FAIL: $*"; fail=1; fi; }

need() { command -v "$1" >/dev/null || { echo "missing: $1 — see CLAUDE.md 'Local setup'"; exit 2; }; }
need node; need go; need python3

# Resolve the interpreter up front (same as tests/ui_gallery.sh) so a pyenv
# pin mismatch is reported clearly instead of the UI suite silently skipping
# below (see issue #118). --fast doesn't need Playwright at all.
py=$(python3 -c 'import sys; print(sys.executable)')
if [[ $FAST -eq 0 ]] && ! "$py" -c 'import playwright' 2>/dev/null; then
    echo "playwright missing in $py — pin the interpreter with playwright" \
         "(.python-version / PYENV_VERSION) or install:" \
         "$py -m pip install --user playwright==1.60.0 && $py -m playwright install chromium"
    exit 2
fi

step "snapshot-history guard self-test"
run tests/test_check_snapshot_history.sh

step "snapshot-history guard (this branch vs origin/master)"
run scripts/check-snapshot-history.sh

step "web builder unit tests (node)"
run node --test 'tests/web_unit/*.test.mjs'

step "go bridge (skips server-backed cases without a Linux pgwt-server)"
run bash -c 'cd web && go vet ./... && go test ./...'

step "python: compile every test module"
run python3 -m py_compile tests/*.py

step "python: free_ports self-test"
run python3 tests/test_free_ports.py

# Issue #214: tests/demo_workload_coverage.py's per-tab *_populated()
# checkers are pure (no browser, no network, no PG) -- same reasoning as
# the demo-rehearsal bypass-suite step below, run in the fast tier rather
# than waiting for an actual box capture to exercise them.
step "python: demo-workload-coverage unit tests"
run python3 tests/test_demo_workload_coverage.py

# Owner finding, 2026-09-28 (run.id 1790574871): the demo-rehearsal bypass
# suite (tests/test_demo_rehearsal_lib.py) used to run only in the
# box-check/CI tier (tests/unit_tests.list), so a regression in the
# rehearsal's own gating logic -- e.g. its blink verdict silently
# diverging from ui_live_smoke_lib.build_tab_result's -- was only ever
# caught by an actual 30-45 minute rehearsal run, not by `make check`.
# Pure Python, no browser, no network, no Linux/root needed -- belongs in
# the fast deterministic tier.
step "python: demo-rehearsal bypass-suite unit tests"
run python3 tests/test_demo_rehearsal_lib.py

# #243: Workload.fire()'s verify flag and live_loop_workload.py's periodic
# verify cadence -- pure Python, fakes psql/sessions, no Linux/root/live-DB
# needed. Belongs in the fast deterministic tier for the same reason as the
# bypass suite above: a regression here (verify silently ignored, or the
# cadence silently reverting to "always" or "never") is otherwise only
# caught by an actual live capture's Sessions-tab row count or, worse, by
# the re-lock loop breaking unnoticed.
step "python: Workload.fire() verify-flag/cadence unit tests (#243)"
run python3 tests/test_workload_verify_flag.py

# Same reasoning as the bypass-suite step above, same owner rule (a
# Mac-side script's unit tests must not live only in the box tier): the
# PG-version gate in scripts/demo-rehearsal.sh (parse_pg_probe_line,
# pg_version_verdict -- the check that refuses a rehearsal run against the
# wrong PostgreSQL major) lives entirely in
# tests/demo_rehearsal_orchestrator_lib.py, pure Python with no browser/
# network/Linux dependency. Without this step a regression there would not
# surface until the next box run.
step "python: demo-rehearsal orchestrator unit tests"
run python3 tests/test_demo_rehearsal_orchestrator_lib.py

step "python: gallery provenance and tiered output unit tests"
run python3 tests/test_evidence_output.py

step "CI change-classifier table-driven test (issue #167)"
run bash tests/test_classify_changed_files.sh

step "cross-validate rate-sweep gate unit tests (issue #309)"
run bash tests/test_cross_validate_rate_gate.sh

if [[ $FAST -eq 0 ]]; then
    # ── Port allocation (so two `make check` runs on this Mac don't collide) ─
    # One free base per run, laid out at FIXED OFFSETS below so every mock
    # server this run spawns gets a run-private port; no lock (that would
    # serialize two ~4-minute runs), see tests/free_ports.py. Allocated here,
    # immediately before the mocks bind, to keep the probe-to-bind window as
    # short as possible. A developer running one suite by hand (no env vars
    # set) keeps today's fixed-default ports. Safe across worktrees; NOT safe
    # for two `make check` runs in the SAME checkout at once (those still
    # race on .pgwt-check.stamp and tests/results/).
    #
    #   PORT MAP (relative to PGWT_PORT_BASE):
    #     PGWT_TEST_PORT  = base+0   test_web_ui.py:       HTTP +0/WS +1, B5 +10/+11, compare +20/+21
    #     PGWT_CHAOS_PORT = base+30  test_web_ui_chaos.py: HTTP +30/WS +31
    #     PGWT_SNAP_PORT  = base+40  reserved for when check.sh runs the
    #                                snapshot suite (test_web_ui_snapshots.py
    #                                isn't invoked here yet): HTTP +40/WS +41,
    #                                sampled +50/+51
    #     PGWT_CHIP_PORT  = base+60 test_chip_label_alignment.py: HTTP +60
    #                                (gallery.html is static — no WS pair)
    PORT_SPAN=70
    PGWT_PORT_BASE=$(python3 tests/free_ports.py "$PORT_SPAN") || { echo "free_ports: could not allocate $PORT_SPAN free ports"; exit 2; }
    [[ $PGWT_PORT_BASE =~ ^[0-9]+$ ]] || { echo "free_ports: non-numeric base '$PGWT_PORT_BASE'"; exit 2; }
    export PGWT_TEST_PORT=$PGWT_PORT_BASE
    export PGWT_CHAOS_PORT=$((PGWT_PORT_BASE + 30))
    export PGWT_SNAP_PORT=$((PGWT_PORT_BASE + 40))
    export PGWT_CHIP_PORT=$((PGWT_PORT_BASE + 60))
    echo "port base: $PGWT_PORT_BASE (span $PORT_SPAN — TEST=+0 CHAOS=+30 SNAP=+40 CHIP=+60)"

    step "web UI suite vs mock_server.py (Playwright)"
    run python3 tests/test_web_ui.py
    step "web UI chaos suite (latency jitter / reconnects)"
    run python3 tests/test_web_ui_chaos.py
    step "overlay chip label alignment at DSF 1/2 (issue #213)"
    run python3 tests/test_chip_label_alignment.py

    # issue #245 (TTFP): _navigate_to_tab's ttfp_ms anchor and its wiring
    # into run_tab's build_tab_result call -- imports ui_live_smoke.py,
    # which hard-requires Playwright, so this runs here (already guarded
    # above) rather than via tests/unit_tests.list (that list's CI job
    # installs no Playwright on purpose -- see
    # test_demo_workload_coverage.py's test_import_needs_no_playwright).
    step "ui_live_smoke navigation TTFP anchor (issue #245)"
    run python3 tests/test_ui_live_smoke_nav.py
fi

if [[ $fail -ne 0 ]]; then
    echo; echo "CHECK FAILED"; rm -f .pgwt-check.stamp; exit 1
fi

# Stamp the exact tree (tracked + untracked, minus ignored) that passed.
scripts/tree-hash.sh > .pgwt-check.stamp
echo; echo "CHECK PASSED ($( [[ $FAST -eq 1 ]] && echo fast || echo full )) — stamp $(cat .pgwt-check.stamp)"
[[ $FAST -eq 1 ]] && echo "note: --fast skips the UI suite; the push guard accepts it, CI does not."
exit 0
