#!/bin/bash
# run_all.sh — Master test runner for pg_wait_tracer
# Usage: sudo tests/run_all.sh [--pid POSTMASTER_PID] [--pg-version N] [--require-live]
#
# --require-live: any skip in the root+PG live section is a FAILURE. This is
# the gate for capture-behavior phases (Trust Milestone standing rule): an
# all-skip run used to exit 0 (TST-4), which is how "green" runs happened on
# boxes where nothing live actually executed. Use it on the real test box.
#
# PGPORT: derived automatically from the resolved postmaster's own
# postmaster.pid (falling back to the multi-PG gate box's port convention,
# cluster N -> port PGWT_PG_PORT_BASE+N, only if that can't be read). This
# makes the run self-sufficient — it no longer depends on the caller's/box's
# ambient PGPORT, which used to be left over from whichever cluster a
# previous run last targeted. Set PGWT_PGPORT to override explicitly.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
source "$SCRIPT_DIR/testutil.sh"

# issue #93: this run's marker (its own start timestamp), exported so
# tests/ui_live_smoke.py can stamp tests/results/ui_live/run.id with it.
# tests/results is excluded from box-check.sh's up-rsync, so the box keeps
# the LAST run's ui_live/ between invocations -- without this, a smoke that
# died before ever reaching the walk would have this script re-emit the
# previous (unrelated) run's PASS/FAIL verdict as if it were this run's.
export PGWT_RUN_MARKER="$(date +%s)"

PM_PID=""
PG_VERSION=""
PG_VERSION_EXPLICIT=0
REQUIRE_LIVE=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --pid) PM_PID="$2"; shift 2 ;;
        --pg-version) PG_VERSION="$2"; PG_VERSION_EXPLICIT=1; shift 2 ;;
        --require-live) REQUIRE_LIVE=1; shift ;;
        *) echo "Usage: $0 [--pid POSTMASTER_PID] [--pg-version N] [--require-live]"; exit 1 ;;
    esac
done

# Auto-detect postmaster PID if not provided
if [[ -z "$PM_PID" ]] && pgrep -x postgres > /dev/null 2>&1; then
    if [[ -n "$PG_VERSION" ]]; then
        PM_PID=$(find_postmaster --pg-version "$PG_VERSION")
    else
        PM_PID=$(find_postmaster)
    fi
    if [[ -n "$PM_PID" ]]; then
        local_ver=$(postmaster_version "$PM_PID" || true)
        echo "Auto-detected postmaster PID $PM_PID (PostgreSQL ${local_ver:-unknown})"
    fi
fi

# A caller-requested --pg-version that matched no running postmaster must
# refuse loudly, not silently fall through with PID_ARG empty (that used to
# make every live test skip instead of failing the run the caller actually
# asked for).
if [[ "$PG_VERSION_EXPLICIT" -eq 1 && -z "$PM_PID" ]]; then
    echo "ERROR: --pg-version $PG_VERSION requested but no running PostgreSQL $PG_VERSION postmaster was found" >&2
    exit 1
fi

PID_ARG=""
if [[ -n "$PM_PID" ]]; then
    PID_ARG="--pid $PM_PID"
fi

# PG_VERSION drives LIVE_TESTS' per-test minimum-version gating (Step 4)
# below. --pg-version already sets it; an explicit --pid or a bare
# auto-detect (no --pg-version) both leave it unset, so derive it from the
# resolved postmaster's binary path via the same testutil.sh helper
# find_postmaster uses (handles non-Debian layouts too, e.g. RPM's
# /usr/pgsql-<N>/bin/postgres, via its `postgres --version` fallback).
if [[ -z "$PG_VERSION" && -n "$PM_PID" ]]; then
    PG_VERSION=$(postmaster_version "$PM_PID" || true)
fi

# PGPORT: --pid/find_postmaster above only pick which postmaster gets
# *traced*. The workload side (pgbench/psql invoked with no explicit -p by
# the tests below) instead routes through Debian's pg_wrapper via this env
# var alone, so a stale PGPORT left over from a previous run silently
# traces one cluster while the load runs against another — found on the
# gate box: a PG13 run traced PG13 correctly but every capture came back
# CPU*-only/empty because the workload was hitting whatever cluster
# PGPORT happened to point at (see docs/DEV_LOOP_PLAN.md step 1). Read the
# port directly from the resolved postmaster's own postmaster.pid (line 4)
# via its /proc/$PM_PID/cwd (postmaster's CWD is its data directory) — this
# is authoritative and works for any layout, including a stock
# single-cluster host on 5432, not just this box's 54<major> convention.
# Fall back to that convention only if postmaster.pid can't be read.
# PGWT_PGPORT lets a caller override either explicitly.
PID_PORT=""
if [[ -n "$PM_PID" ]]; then
    PM_CWD=$(readlink "/proc/$PM_PID/cwd" 2>/dev/null || true)
    if [[ -n "$PM_CWD" && -f "$PM_CWD/postmaster.pid" ]]; then
        PID_PORT=$(sed -n '4p' "$PM_CWD/postmaster.pid" 2>/dev/null || true)
        [[ "$PID_PORT" =~ ^[0-9]+$ ]] || PID_PORT=""
    fi
fi

PG_PORT_BASE="${PGWT_PG_PORT_BASE:-5400}"
if [[ -n "${PGWT_PGPORT:-}" ]]; then
    export PGPORT="$PGWT_PGPORT"
    echo "PGPORT=$PGPORT (caller override via PGWT_PGPORT)"
elif [[ -n "$PID_PORT" ]]; then
    export PGPORT="$PID_PORT"
    echo "PGPORT=$PGPORT (read from postmaster.pid for PID $PM_PID)"
elif [[ -n "$PG_VERSION" ]]; then
    export PGPORT=$((PG_PORT_BASE + PG_VERSION))
    echo "PGPORT=$PGPORT (derived: PGWT_PG_PORT_BASE=$PG_PORT_BASE + PG$PG_VERSION, postmaster.pid unreadable)"
fi

passed=0
failed=0
skipped=0
excluded=0
known_failing=0
xpass=0

# KNOWN_FAILING: test name -> tracking issue number. ONLY for a test that
# reproduces a real, filed product bug (e.g. #97, #98) — never for timing or
# runner noise; a noisy test gets moved to a different tier or investigated,
# it is never silenced here (see also CLAUDE.md's Rules).
#
# A listed test still runs in full, every time, and NEITHER outcome fails
# the gate: an expected failure is printed loudly as "KNOWN-FAILING
# (issue #N)"; an unexpected pass — the bug can be intermittent (it may fail
# under one PG version's test sequence and pass under another) or already
# fixed — is printed as "UNEXPECTED PASS (issue #N) — intermittent or fixed;
# check the issue". Both are counted in the known_failing bucket; passes are
# additionally counted as `xpass` so the summary line (e.g.
# "known-failing 2 (1 xpass)") tells a reviewer to go re-check the issue
# rather than assume the list is stale from the exit code alone. A listed
# test that could not even run (exit 126/127: not executable, or the file
# is missing) is a broken test, not the tracked bug — that always counts as
# a real failure regardless of KNOWN_FAILING membership.
KNOWN_FAILING=(
    "test_partition|99"
)

# known_failing_issue NAME — prints the tracking issue number and returns 0
# if NAME is listed in KNOWN_FAILING, else returns 1 with no output.
known_failing_issue() {
    local target="$1"
    local entry name issue
    for entry in "${KNOWN_FAILING[@]}"; do
        IFS='|' read -r name issue <<< "$entry"
        if [[ "$name" == "$target" ]]; then
            echo "$issue"
            return 0
        fi
    done
    return 1
}

run_test() {
    local name="$1"
    shift
    echo ""
    echo "════════════════════════════════════════"
    echo "  $name"
    echo "════════════════════════════════════════"
    local rc=0
    "$@" || rc=$?
    local issue
    if issue=$(known_failing_issue "$name"); then
        # A listed test that could not even run (126: found but not
        # executable, 127: command/file not found) is not the known product
        # bug being tracked — it's a broken test, and must fail the gate
        # like any other test, not disappear into known_failing.
        if [[ "$rc" -eq 126 || "$rc" -eq 127 ]]; then
            echo "  FAILED TO RUN (exit $rc) — not the tracked bug (issue #$issue), counts as a real failure"
            failed=$((failed + 1))
        elif [[ "$rc" -eq 0 ]]; then
            echo "  UNEXPECTED PASS (issue #$issue) — intermittent or fixed; check the issue"
            known_failing=$((known_failing + 1))
            xpass=$((xpass + 1))
        else
            echo "  KNOWN-FAILING (issue #$issue)"
            known_failing=$((known_failing + 1))
        fi
    elif [[ "$rc" -eq 0 ]]; then
        passed=$((passed + 1))
    else
        failed=$((failed + 1))
    fi
}

skip_test() {
    local name="$1"
    local reason="$2"
    echo ""
    echo "════════════════════════════════════════"
    echo "  $name — SKIPPED ($reason)"
    echo "════════════════════════════════════════"
    skipped=$((skipped + 1))
}

# Skip in the root+PG LIVE section. Under --require-live a live skip is a
# FAILURE, not a skip — the live suite silently not running is exactly how
# all four field escapes (#8/#24/#30/#31) shipped (TST-4).
skip_live_test() {
    local name="$1"
    local reason="$2"
    if [[ $REQUIRE_LIVE -eq 1 ]]; then
        echo ""
        echo "════════════════════════════════════════"
        echo "  $name — FAILED (--require-live set, but: $reason)"
        echo "════════════════════════════════════════"
        failed=$((failed + 1))
    else
        skip_test "$name" "$reason"
    fi
}

# A test whose documented/known minimum PostgreSQL major is above the one
# actually running is EXCLUDED from the matrix, not skipped: it never had a
# chance to run on this target, so it is not the kind of "did the live suite
# even execute" gap --require-live guards against (skip_live_test), and it
# is not a deterministic-tier absence either (skip_test). Printed loudly so
# a shrinking matrix is visible, never silent.
exclude_test() {
    local name="$1"
    local reason="$2"
    echo ""
    echo "════════════════════════════════════════"
    echo "  $name — EXCLUDED ($reason)"
    echo "════════════════════════════════════════"
    excluded=$((excluded + 1))
}

# Step 0: Build main project if needed
echo "Building main project..."
make -C "$PROJECT_DIR" -q 2>/dev/null || make -C "$PROJECT_DIR"

# Step 1: Build C unit tests
echo "Building C unit tests..."
make -C "$SCRIPT_DIR"

# Step 2: C unit tests (no root needed).
# The list lives in unit_tests.list — the SINGLE source of truth shared with
# CI (`make -C tests check`). Never add unit tests here directly (TST-3).
# Run from tests/ (cd, then restore CWD) exactly like `make -C tests check`
# does: some binaries (e.g. test_effective_cores) resolve fixtures via a path
# relative to CWD, and the two *.py entries are tracked without the +x bit,
# so they need an explicit `python3` prefix rather than direct exec — same
# two rules tests/Makefile's `check` target already applies.
# (read via fd 3 so the tests' stdin stays the terminal, not the list file)
pushd "$SCRIPT_DIR" >/dev/null
while read -r t <&3; do
    case "$t" in ''|\#*) continue ;; esac
    case "$t" in
        *.py) run_test "$t" python3 "$t" ;;
        *)    run_test "$t" ./"$t" ;;
    esac
done 3< "unit_tests.list"
popd >/dev/null

# Step 2.5: Synthetic data correctness tests (no root needed, needs pgwt-server)
if [[ -x "$PROJECT_DIR/pgwt-server" ]] && [[ -x "$SCRIPT_DIR/gen_test_traces" ]]; then
    run_test "test_data_time_model" python3 "$SCRIPT_DIR/test_data_time_model.py"
    run_test "test_data_offcpu_identity" python3 "$SCRIPT_DIR/test_data_offcpu_identity.py"
    run_test "test_data_aas" python3 "$SCRIPT_DIR/test_data_aas.py"
    run_test "test_data_events" python3 "$SCRIPT_DIR/test_data_events.py"
    run_test "test_data_sessions" python3 "$SCRIPT_DIR/test_data_sessions.py"
    run_test "test_data_queries" python3 "$SCRIPT_DIR/test_data_queries.py"
    run_test "test_data_filters" python3 "$SCRIPT_DIR/test_data_filters.py"
    run_test "test_data_timeline" python3 "$SCRIPT_DIR/test_data_timeline.py"
    run_test "test_data_idle" python3 "$SCRIPT_DIR/test_data_idle.py"
    run_test "test_data_edge" python3 "$SCRIPT_DIR/test_data_edge.py"
    run_test "test_data_transitions" python3 "$SCRIPT_DIR/test_data_transitions.py"
    run_test "test_data_lock_chains" python3 "$SCRIPT_DIR/test_data_lock_chains.py"
    run_test "test_data_fidelity" python3 "$SCRIPT_DIR/test_data_fidelity.py"
    run_test "test_data_esc_coverage" python3 "$SCRIPT_DIR/test_data_esc_coverage.py"
    run_test "test_data_summary_honesty" python3 "$SCRIPT_DIR/test_data_summary_honesty.py"
    run_test "test_data_markers" python3 "$SCRIPT_DIR/test_data_markers.py"
    run_test "test_data_executions" python3 "$SCRIPT_DIR/test_data_executions.py"
    run_test "test_data_categories" python3 "$SCRIPT_DIR/test_data_categories.py"
    run_test "test_data_window_bound" python3 "$SCRIPT_DIR/test_data_window_bound.py"
    run_test "test_current_trace" python3 "$SCRIPT_DIR/test_current_trace.py"
    run_test "test_protocol_drift" python3 "$SCRIPT_DIR/test_protocol_drift.py"
else
    skip_test "test_data_*" "pgwt-server or gen_test_traces not built"
fi

# Live test inventory (root + running PG). One list, used both to run and to
# report skips — keep names and runners in sync here only.
# test_overhead runs in --quick mode (~10 min instead of ~70) and appends a
# CSV trend row set to tests/results/overhead_trend.csv; a full sweep is
# still available via `sudo tests/test_overhead.sh` directly.
run_overhead_quick() {
    local results_dir="$SCRIPT_DIR/results"
    local trend="$results_dir/overhead_trend.csv"
    local tmp_csv
    tmp_csv=$(mktemp /tmp/pgwt_overhead_XXXXXX.csv)
    mkdir -p "$results_dir"
    if bash "$SCRIPT_DIR/test_overhead.sh" --quick --output "$tmp_csv" $PID_ARG; then
        local status=0
    else
        local status=1
    fi
    # Append per-run rows to the tracked trend file (date + commit prefixed)
    if [[ -s "$tmp_csv" ]]; then
        if [[ ! -f "$trend" ]]; then
            echo "date,commit,clients,mode,run,tps" > "$trend"
        fi
        local when commit
        when=$(date -u +%Y-%m-%dT%H:%M:%SZ)
        commit=$(git -C "$PROJECT_DIR" rev-parse --short HEAD 2>/dev/null || echo unknown)
        tail -n +2 "$tmp_csv" | sed "s|^|$when,$commit,|" >> "$trend"
        echo "Overhead trend appended to $trend"
    fi
    rm -f "$tmp_csv"
    return $status
}

# Step 3: CLI tests (needs root; PG optional for the arg-validation checks)
if [[ $(id -u) -eq 0 ]]; then
    run_test "test_cli" bash "$SCRIPT_DIR/test_cli.sh" $PID_ARG
else
    skip_live_test "test_cli" "requires root"
fi

# Fourth field (optional): minimum PostgreSQL major this test is known to
# need. Determined empirically on the gate box (isolated re-runs against
# each provisioned major, not just the documented "Requires: PG18"
# docstrings, most of which turned out to be aspirational rather than a
# real constraint — see docs/DEV_LOOP_PLAN.md step 1). Under --pg-version N
# below a test's minimum, exclude_test() takes it out of the matrix instead
# of running (and failing) it.
LIVE_TESTS=(
    "test_lifecycle|bash|test_lifecycle.sh"
    "test_cross_validate|python3|test_cross_validate.py"
    # pg_stat_io (Test 3's IO cross-check) is a PG16+ view; empirically
    # fails on PG13, passes on PG17/18.
    "test_accuracy|python3|test_accuracy.py|17"
    "test_deterministic|python3|test_deterministic.py"
    "test_overhead|overhead_quick|"
    "test_client_wait|python3|test_client_wait.py"
    "test_cpu_time|python3|test_cpu_time.py"
    "test_lwlock|python3|test_lwlock.py"
    # compute_query_id is a PG14+ GUC (tests/provision-runner.sh only sets it
    # for 14+); test_query_event asserts on it directly and errors out on
    # PG13 (empirically reproducible). PG13 query attribution itself stays
    # covered separately by test_capture_smoke's PG13 branch (synthetic
    # grouping keys, no compute_query_id dependency).
    "test_query_event|python3|test_query_event.py|14"
    "test_cross_pg_wait_sampling|python3|test_cross_pg_wait_sampling.py"
    "test_event_classes|python3|test_event_classes.py"
    "test_multi_window|python3|test_multi_window.py"
    "test_active|python3|test_active.py"
    # Live data correctness tests (Sprint 4)
    "test_percentage|python3|test_percentage.py"
    "test_aas_accuracy|python3|test_aas_accuracy.py"
    # Empirically fails on PG13 (isolated re-run, reproducible), passes on
    # PG17/18.
    "test_session_accuracy|python3|test_session_accuracy.py|17"
    "test_query_accuracy|python3|test_query_accuracy.py|17"
    "test_partition|python3|test_partition.py"
    "test_idle_exclusion|python3|test_idle_exclusion.py"
    "test_daemon_server|python3|test_daemon_server.py"
    "test_control|bash|test_control.sh"
    "test_escalation|bash|test_escalation.sh"
    # Tiered-capture live tests (were built for A4/A5 but wired into
    # nothing — TST-7). test_cross_validate_tiered is the test that
    # justified tiered as the default mode.
    "test_cross_validate_tiered|bash|test_cross_validate_tiered.sh"
    "test_anomaly_live|bash|test_anomaly_live.sh"
    # T4/CAP-1: a full BPF state_map must be loud (metrics + ERROR log)
    "test_state_map_loud|python3|test_state_map_loud.py"
    # issue #93: live UI smoke — walks all 11 tabs against a REAL daemon +
    # Go bridge (not mock_server.py). Needs Playwright + Chromium on the box
    # (tests/provision-runner.sh); prints its own PASS/FAIL summary line and
    # writes tests/results/ui_live/summary.json (phase 2: scripts/box-check.sh
    # rsyncs that directory back to the Mac).
    "test_ui_live_smoke|bash|ui_live_smoke.sh"
)

# Step 4: integration + live-correctness tests (root + running PG)
if [[ $(id -u) -eq 0 ]] && pgrep -x postgres > /dev/null 2>&1; then
    for entry in "${LIVE_TESTS[@]}"; do
        IFS='|' read -r name runner file min_pg <<< "$entry"
        if [[ -n "$min_pg" && -n "$PG_VERSION" && "$PG_VERSION" -lt "$min_pg" ]]; then
            exclude_test "$name" "needs PostgreSQL >= $min_pg, running PG$PG_VERSION"
            continue
        fi
        case "$runner" in
            bash)           run_test "$name" bash "$SCRIPT_DIR/$file" $PID_ARG ;;
            python3)        run_test "$name" python3 "$SCRIPT_DIR/$file" $PID_ARG ;;
            overhead_quick) run_test "$name (--quick)" run_overhead_quick ;;
        esac
    done
else
    if [[ $(id -u) -ne 0 ]]; then
        live_skip_reason="requires root"
    else
        live_skip_reason="PostgreSQL not running"
    fi
    for entry in "${LIVE_TESTS[@]}"; do
        IFS='|' read -r name _ _ _ <<< "$entry"
        skip_live_test "$name" "$live_skip_reason"
    done
fi

# Step 4.5: Web-UI builder unit tests (Node, no browser — Track U U0). The
# same layer CI's build-and-unit job runs: pure builders and the state/
# transport/selection modules fail here in milliseconds, no Playwright needed.
# Locally a missing node is a skip; in CI ($CI set) it is a FAILURE.
if command -v node >/dev/null 2>&1; then
    run_test "web_unit (node --test)" node --test "$SCRIPT_DIR"/web_unit/*.test.mjs
elif [[ -n "${CI:-}" ]]; then
    run_test "web_unit (node --test)" bash -c \
        'echo "ERROR: node not installed — required in CI"; exit 1'
else
    skip_test "web_unit (node --test)" "node not installed"
fi

# Step 5: Web UI tests (needs playwright + websockets, no root needed).
# OFF by default (PGWT_RUN_WEB_UI=1 opts in): this duplicates `make check`'s
# own Playwright suite (the Mac) and ci.yml's dedicated web-ui job, and since
# the gate box now has Playwright+Chromium (tests/provision-runner.sh, issue
# #93), leaving it on by default promoted this into every `make box-check`
# run — a real Chromium walk the shared box does not need to also carry.
# No separate "$CI" hard-fail branch here (unlike Steps elsewhere in this
# file): run_all.sh is a box-check/manual-box script, never invoked by any
# GitHub Actions workflow directly (ci.yml's web-ui job runs test_web_ui.py
# itself), so $CI is never actually set for this script -- that branch was
# unreachable dead code. test_web_ui_chaos runs the same UI against
# mock_server.py in CHAOS mode (latency jitter / out-of-order / late
# responses) — its race-exposing tests are classified gating vs xfail
# internally, so it stays green either way once opted in.
if [[ "${PGWT_RUN_WEB_UI:-}" != "1" ]]; then
    skip_test "test_web_ui" "opt-in: set PGWT_RUN_WEB_UI=1 (make check + ci.yml's web-ui job already cover this)"
    skip_test "test_web_ui_chaos" "opt-in: set PGWT_RUN_WEB_UI=1 (make check + ci.yml's web-ui job already cover this)"
elif python3 -c "import playwright, websockets" 2>/dev/null; then
    run_test "test_web_ui" python3 "$SCRIPT_DIR/test_web_ui.py"
    run_test "test_web_ui_chaos" python3 "$SCRIPT_DIR/test_web_ui_chaos.py"
else
    skip_test "test_web_ui" "opted in via PGWT_RUN_WEB_UI=1 but playwright or websockets not installed"
    skip_test "test_web_ui_chaos" "opted in via PGWT_RUN_WEB_UI=1 but playwright or websockets not installed"
fi

# Visual-regression snapshots (Phase B4). Needs playwright + Pillow + numpy AND
# committed baselines (tests/web_snapshots/*.png). Baselines are environment-
# specific (generated in CI's chromium, see tests/web_snapshots/VERSION), so
# the authoritative gating run is the dedicated CI `snapshots` job — which
# invokes the suite directly, not via this script. On a dev box the compare
# mostly churns font diffs, so it is OPT-IN here via PGWT_RUN_SNAPSHOTS=1
# (Track U U0); without it this is a skip, never a spurious local red.
if [[ "${PGWT_RUN_SNAPSHOTS:-}" != "1" ]]; then
    skip_test "test_web_ui_snapshots" "opt-in: set PGWT_RUN_SNAPSHOTS=1 (CI snapshots job is authoritative)"
elif python3 -c "import playwright, websockets, PIL, numpy" 2>/dev/null \
   && ls "$SCRIPT_DIR"/web_snapshots/*.png >/dev/null 2>&1; then
    run_test "test_web_ui_snapshots" python3 "$SCRIPT_DIR/test_web_ui_snapshots.py"
else
    skip_test "test_web_ui_snapshots" "snapshot deps or baselines not present (CI snapshots job is authoritative)"
fi

# issue #93: re-emit the live-UI-smoke's own one-line verdict here too, not
# just buried in its own (possibly thousands-of-lines) run_test block above —
# CLAUDE.md's definition of done says `make box-check`'s summary (the last
# ~25 lines an agent pastes into a PR) shows this; without it, a reviewer
# reading only the tail never sees per-tab known-failing/xpass status.
#
# Gated on tests/results/ui_live/run.id == $PGWT_RUN_MARKER: box-check.sh's
# up-rsync excludes tests/results, so the box keeps the LAST run's ui_live/
# between invocations -- without this check, a smoke that died before ever
# reaching the walk (e.g. the daemon/bridge never came up) would silently
# re-emit a PREVIOUS, unrelated run's PASS here, right next to THIS run's
# `failed 1` for the very same test.
ui_live_dir="$SCRIPT_DIR/results/ui_live"
python3 - "$ui_live_dir/summary.json" "$ui_live_dir/run.id" "$PGWT_RUN_MARKER" <<'PYEOF'
import json
import sys

summary_path, run_id_path, expected_marker = sys.argv[1], sys.argv[2], sys.argv[3]

try:
    actual_marker = open(run_id_path).read().strip()
except OSError:
    actual_marker = None

if actual_marker != expected_marker:
    print("  Live UI smoke: NOT RUN in this invocation")
    sys.exit(0)

try:
    s = json.load(open(summary_path))
except Exception as e:
    print(f"  Live UI smoke: could not read summary.json ({e})")
    sys.exit(0)

line = "  Live UI smoke: overall=" + ("PASS" if s.get("ok") else "FAIL")
if s.get("failed_tabs"):
    line += " (failed: " + ", ".join(s["failed_tabs"]) + ")"
if s.get("known_failing_tabs"):
    line += " (known-failing: " + ", ".join(s["known_failing_tabs"]) + ")"
if s.get("xpass_tabs"):
    line += " (xpass: " + ", ".join(s["xpass_tabs"]) + ")"
print(line)
PYEOF

# Summary
echo ""
echo "════════════════════════════════════════"
echo "  SUMMARY"
echo "════════════════════════════════════════"
total=$((passed + failed + skipped + excluded + known_failing))
executed=$((passed + failed + known_failing))
echo "  Executed: $executed (passed $passed, failed $failed, known-failing $known_failing ($xpass xpass)), skipped $skipped, excluded $excluded, total $total"
if [[ $REQUIRE_LIVE -eq 1 ]]; then
    echo "  Mode:     --require-live (live-section skips counted as failures)"
fi
echo ""

# An all-skip run must never be green: something is wrong with the
# environment or the runner itself if nothing executed (TST-4).
if [[ $executed -eq 0 ]]; then
    echo "ERROR: no test was executed — refusing to report success"
    exit 1
fi

[[ $failed -eq 0 ]]
