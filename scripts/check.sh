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

# Same family as the snapshot-history guard: both protect the integrity of
# the .pgwt-check.stamp this script writes at the end. A build product that
# .gitignore does not cover is untracked-but-unignored, so scripts/tree-hash.sh
# counts it and the stamp stops matching the moment anyone runs
# `make -C tests` -- which is how demo-rehearsal counted attempt 2 lost its
# pairing. Lives HERE and not in tests/unit_tests.list on purpose: box-check
# rsyncs the tree WITHOUT .git, so on the box this guard could only refuse for
# an environmental reason, and a gate that is red for the wrong reason gets
# switched off. This Mac and CI are the places it is both meaningful and able
# to run.
step "bash: .gitignore covers every tests/ build product (tree-hash integrity)"
run bash tests/test_gitignore_build_products.sh

# The summary format version is the one number in the docs with an
# OPERATIONAL consequence: DEMO_RUNBOOK.md tells the operator which trace
# directories to move aside, and a stale N leaves a directory the server
# then refuses on the summary fast path -- the 900 s Overview silently
# drops to raw, which is the paint latency the runbook exists to avoid.
# It drifted twice in one night (v6 while the code was 8; then a v8
# correction landing on a tree that had merged v9), so it is checked rather
# than proofread.
step "bash: the docs name the PGWT_SUMMARY_VERSION the code defines"
run bash tests/test_docs_summary_version.sh

step "snapshot-history guard self-test"
run tests/test_check_snapshot_history.sh

step "snapshot-history guard (this branch vs origin/master)"
run scripts/check-snapshot-history.sh

# Link-closure guard (#424, 2026-10-15). Adding one call to src/map_reader.c
# compiled everywhere and broke the LINK of test_event_reader and
# test_concurrent_rw on Linux, because both link map_reader.o for its
# accumulator core alone. It cost two ephemeral VMs and two `box-check` exits
# before anyone knew, with every test that ran passing. "It compiles" cannot
# see that; comparing the TU's undefined-symbol set against the merge base
# can, in about a second, here. The guard REFUSES (and so fails this step)
# when it cannot resolve a base, when the base revision will not compile,
# when the compiler is missing, or when the object it built is not the TU it
# claims -- a closure check with no baseline approves everything.
step "link-closure guard self-test"
run tests/test_check_link_closure.sh

step "link-closure guard (src/map_reader.c vs merge base)"
run scripts/check-link-closure.sh

# Runner-registration guard (#433): tests/run_all.sh (make box-check) and
# tests/ci_smoke.sh (CI/nightly) have independent membership, so a test
# wired into ci_smoke.sh alone makes box-check "green" prove nothing about
# it (three real incidents: test_v9_open_cpu_wired registered ci_smoke-only
# twice over, and test_capture_smoke mentioned in run_all.sh only inside a
# comment while ci_smoke.sh actually runs it). Only the SELF-TEST (synthetic
# fixtures) runs here, not scripts/check-runner-registration.sh itself
# against this real repo: that live run is a reviewer-run-by-hand evidence
# guard, same as scripts/check-test-registration.sh -- wiring it into every
# `make check` would turn a real, already-filed, out-of-scope registration
# gap (test_capture_smoke.py / test_uprobe_fired.py, tracked by #433, not
# fixed here since fixing it means editing tests/run_all.sh, a live-tier
# test-assertion change) into a permanent red for every unrelated branch.
step "runner-registration guard self-test"
run tests/test_check_runner_registration.sh

# Runner-image pin guard (2026-10-22). GitHub rolled `ubuntu-latest` to Ubuntu
# 26.04 and four gating jobs went red at once (run 39765852420): Playwright
# 1.60.0 has no chromium for ubuntu26.04-x64, setup-python handed the
# v1.60.0-noble container a Python with no working ssl module, and the newer
# GCC made tests/gen_test_traces.c's implicit unlink() declaration fatal. The
# Playwright version is pinned to the one the 47 committed snapshot baselines
# were rendered with, so the image is pinned instead -- and this guard refuses
# a tree that drifts back to a floating label, or that loses
# -Werror=implicit-function-declaration (the flag that keeps the
# implicit-declaration class fatal on the older, pinned GCC too).
step "runner-image pin guard self-test (bypass suite)"
run bash tests/test_check_runner_pin.sh

# Promoted from an untracked results directory; closes the numpy-presence
# false-negative in the frame detector's bypass probes (issue #519).
step "frame-verify probe suite (bypass suite for scripts/frame_verify.py, #519)"
run python3 tests/test_frame_verify_probes.py

step "runner-image pin guard (this tree)"
run bash scripts/check-runner-pin.sh

# tests/results/ evidence-directory ignore rule (companion to this guard's
# own commit, same failure class as the tree-hash guards above): an agent
# writing a brand-new tests/results/<name>/ directory must not move
# scripts/tree-hash.sh's output, and a tracked tests/results/ file must
# never become invisible to `git status` by also matching an ignore rule.
step "tests/results/ evidence-directory ignore rule"
run bash tests/test_results_evidence_ignore.sh

step "web builder unit tests (node)"
run node --test 'tests/web_unit/*.test.mjs'

step "go bridge (skips server-backed cases without a Linux pgwt-server)"
run bash -c 'cd web && go vet ./... && go test ./...'

step "python: compile every test module"
run python3 -m py_compile tests/*.py

step "python: multi-window scripted snapshots + refusal tests"
run python3 tests/test_multi_window_lib.py

step "python: free_ports self-test"
run python3 tests/test_free_ports.py

step "python: demo-walk recorder refusal/coverage/crop tests"
run python3 tests/test_demo_walk_recorder.py

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

step "python: rehearsal verdict refusal tests"
run python3 tests/test_rehearsal_verdict_hardening.py

step "python: rehearsal workload lifetime and launch refusals (#543)"
run python3 tests/test_rehearsal_workload_lifetime.py

step "python: live-smoke verdict unit tests"
run python3 tests/test_ui_live_smoke_lib.py

step "python: #537 waterfall-readiness diagnostic refusal tests"
run python3 tests/test_diag_537_waterfall.py

step "python: live-smoke failure diagnostic capture (#544)"
run python3 tests/test_ui_live_smoke_failure.py
run python3 tests/test_ui_live_smoke_failure.py --snapshot-js

# #384: the --diag-data per-frame panel-data probe exists in
# tests/ui_live_smoke.py and is reached from tests/ui_live_smoke.sh, but
# tests/demo_rehearsal.py has its OWN run_tab call site -- so the instrument
# could be complete and still produce nothing during a rehearsal. Asserts the
# whole thread (CLI flag -> _run_passes -> every run_tab call) with run_tab
# replaced by a recorder: no browser, no network, milliseconds.
step "python: harness ownership, box argv, teardown and Overview REDs (#544/#546)"
run python3 tests/test_harness_ownership.py

step "python: rehearsal --diag-data wiring (#384)"
run python3 tests/test_demo_rehearsal_diag.py

# #386: the Off-CPU* residual decomposition's parsers, window aggregation
# and refusal paths (tests/schedstat_lib.py) plus the /proc reader driven
# against a FIXTURE /proc tree -- pure Python, no Linux, no root, no live
# box, so it belongs in this tier rather than waiting for a 35-minute
# rehearsal. Same reasoning as the bypass-suite step above.
step "python: Off-CPU* run_delay decomposition unit + bypass suite (#386)"
run python3 tests/test_schedstat_lib.py

# The summary-vs-raw parity gate's comparison core and its false-negative
# suite (tests/summary_raw_parity_lib.py). The LIVE half needs pgwt-server
# answering a generated capture on both routes, so it runs in
# tests/run_all.sh's synthetic-data tier; this half is pure Python and is
# where that gate would fail SILENTLY -- both sides taking the same route,
# two empty answers agreeing, an absent field read as equal, an empty
# expected-divergence table approving everything. Same reasoning as the
# bypass-suite steps above: a regression in the gate's own logic must not
# need a box run to surface.
step "python: summary-vs-raw parity comparison core + bypass suite (#408/#379/#319)"
run python3 tests/test_summary_raw_parity_lib.py

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

# Hermetic launcher fixtures belong in the same local tier as its orchestrator.
step "python: demo-rehearsal launcher provenance unit tests (scripts/test-demo-rehearsal-provenance.py)"
run python3 scripts/test-demo-rehearsal-provenance.py

# tag_cli_args (tests/live_daemon_lib.sh) threads a counted rehearsal's
# --tag through scripts/demo-rehearsal.sh -> ...-remote-run.sh ->
# tests/demo_rehearsal.sh -> demo_rehearsal.py --tag (docs/
# DEMO_REHEARSAL_CRITERIA.md "a counted attempt names the tag it ran
# against"). Pure bash, no Linux/root/live-PostgreSQL needed -- same
# "Mac-side script's unit tests must not live only in the box tier"
# reasoning as the orchestrator step above.
step "bash: tag_cli_args plumbing unit tests"
run bash tests/test_tag_cli_args.sh

# assert_served_ui_matches_tree (tests/live_daemon_lib.sh) refuses a live-tier
# run whose Go bridge serves a web/static OTHER than the tree under test --
# web/main.go embeds the UI into the binary, so a stale web/pgwt would let a
# walk (or a counted demo rehearsal) pass against the wrong UI and report
# green. Hermetic: a throwaway tree plus a python3 http.server, no Go build,
# no PostgreSQL, no root -- so, like the step above, its unit tests do not
# live only in the box tier. The cases are weighted toward FALSE negatives:
# 404, dead server, empty tree, missing dir and missing python3 must each
# REFUSE ("cannot verify"), never pass.
step "bash: served-asset guard unit tests (stale bridge refusal)"
run bash tests/test_served_asset_guard.sh

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
    #     PGWT_WATERFALL_PORT = base+65 verify_waterfall_fold.py (#415):
    #                                HTTP +65/WS +66 (gap left after CHIP so
    #                                a future WS-pair test can slot into +61)
    #     PGWT_DFG_GEOM_PORT  = base+67 test_dfg_geometry.py: HTTP +67
    #                                (gallery.html is static — no WS pair;
    #                                merge note: agent/web-tables and
    #                                agent/web-graph both independently
    #                                claimed +65 — a genuine collision found
    #                                at integration, not a design choice;
    #                                moved DFG_GEOM here to the next free
    #                                offset rather than picking a "winner")
    PORT_SPAN=70
    PGWT_PORT_BASE=$(python3 tests/free_ports.py "$PORT_SPAN") || { echo "free_ports: could not allocate $PORT_SPAN free ports"; exit 2; }
    [[ $PGWT_PORT_BASE =~ ^[0-9]+$ ]] || { echo "free_ports: non-numeric base '$PGWT_PORT_BASE'"; exit 2; }
    export PGWT_TEST_PORT=$PGWT_PORT_BASE
    export PGWT_CHAOS_PORT=$((PGWT_PORT_BASE + 30))
    export PGWT_SNAP_PORT=$((PGWT_PORT_BASE + 40))
    export PGWT_CHIP_PORT=$((PGWT_PORT_BASE + 60))
    export PGWT_WATERFALL_PORT=$((PGWT_PORT_BASE + 65))
    export PGWT_DFG_GEOM_PORT=$((PGWT_PORT_BASE + 67))
    echo "port base: $PGWT_PORT_BASE (span $PORT_SPAN — TEST=+0 CHAOS=+30 SNAP=+40 CHIP=+60 WATERFALL=+65 DFG_GEOM=+67)"

    step "web UI suite vs mock_server.py (Playwright)"
    run python3 tests/test_web_ui.py
    step "web UI chaos suite (latency jitter / reconnects)"
    run python3 tests/test_web_ui_chaos.py
    step "overlay chip label alignment at DSF 1/2 (issue #213)"
    run python3 tests/test_chip_label_alignment.py
    # #415: the drilled Waterfall readout stays above the fold in the FULL
    # application shell (breadcrumbs/controls/AAS chart included) at the
    # pinned demo viewport -- the isolated fixture gallery is exactly what
    # missed this originally (see the script's own docstring).
    step "Waterfall drilled-readout fold check at the demo viewport (issue #415)"
    run python3 tests/verify_waterfall_fold.py
    step "DFG rendered-geometry guard: real coordinateSystem.dataToPoint, not getOption() (issue #436)"
    run python3 tests/test_dfg_geometry.py

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
