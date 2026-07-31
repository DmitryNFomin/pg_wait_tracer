#!/bin/bash
# run_all.sh — Master test runner for pg_wait_tracer
# Usage: sudo tests/run_all.sh [--pid POSTMASTER_PID] [--pg-version N] [--require-live]
#
# --require-live: any skip in the root+PG live section is a FAILURE. This is
# the gate for capture-behavior phases (Trust Milestone standing rule): an
# all-skip run used to exit 0 (TST-4), which is how "green" runs happened on
# boxes where nothing live actually executed. Use it on the real test box.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
source "$SCRIPT_DIR/testutil.sh"

PM_PID=""
PG_VERSION=""
REQUIRE_LIVE=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --pid) PM_PID="$2"; shift 2 ;;
        --pg-version) PG_VERSION="$2"; shift 2 ;;
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
        local_ver=$(readlink /proc/$PM_PID/exe 2>/dev/null | grep -oP 'postgresql/\K\d+(?=/)' || true)
        echo "Auto-detected postmaster PID $PM_PID (PostgreSQL ${local_ver:-unknown})"
    fi
fi

PID_ARG=""
if [[ -n "$PM_PID" ]]; then
    PID_ARG="--pid $PM_PID"
fi

passed=0
failed=0
skipped=0

run_test() {
    local name="$1"
    shift
    echo ""
    echo "════════════════════════════════════════"
    echo "  $name"
    echo "════════════════════════════════════════"
    if "$@"; then
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

# Step 0: Build main project if needed
echo "Building main project..."
make -C "$PROJECT_DIR" -q 2>/dev/null || make -C "$PROJECT_DIR"

# Step 1: Build C unit tests
echo "Building C unit tests..."
make -C "$SCRIPT_DIR"

# Step 2: C unit tests (no root needed).
# The list lives in unit_tests.list — the SINGLE source of truth shared with
# CI (`make -C tests check`). Never add unit tests here directly (TST-3).
# (read via fd 3 so the tests' stdin stays the terminal, not the list file)
while read -r t <&3; do
    case "$t" in ''|\#*) continue ;; esac
    run_test "$t" "$SCRIPT_DIR/$t"
done 3< "$SCRIPT_DIR/unit_tests.list"

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

LIVE_TESTS=(
    "test_lifecycle|bash|test_lifecycle.sh"
    "test_cross_validate|python3|test_cross_validate.py"
    "test_accuracy|python3|test_accuracy.py"
    "test_deterministic|python3|test_deterministic.py"
    "test_overhead|overhead_quick|"
    "test_client_wait|python3|test_client_wait.py"
    "test_cpu_time|python3|test_cpu_time.py"
    "test_lwlock|python3|test_lwlock.py"
    "test_query_event|python3|test_query_event.py"
    "test_cross_pg_wait_sampling|python3|test_cross_pg_wait_sampling.py"
    "test_event_classes|python3|test_event_classes.py"
    "test_multi_window|python3|test_multi_window.py"
    "test_active|python3|test_active.py"
    # Live data correctness tests (Sprint 4)
    "test_percentage|python3|test_percentage.py"
    "test_aas_accuracy|python3|test_aas_accuracy.py"
    "test_session_accuracy|python3|test_session_accuracy.py"
    "test_query_accuracy|python3|test_query_accuracy.py"
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
)

# Step 4: integration + live-correctness tests (root + running PG)
if [[ $(id -u) -eq 0 ]] && pgrep -x postgres > /dev/null 2>&1; then
    for entry in "${LIVE_TESTS[@]}"; do
        IFS='|' read -r name runner file <<< "$entry"
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
        IFS='|' read -r name _ _ <<< "$entry"
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

# Step 5: Web UI tests (needs playwright + websockets, no root needed)
# Locally a missing dependency is a skip; in CI ($CI set) it is a FAILURE —
# the UI suite silently not running is how regressions slip through.
# test_web_ui_chaos runs the same UI against mock_server.py in CHAOS mode
# (latency jitter / out-of-order / late responses) — its race-exposing tests
# are classified gating vs xfail internally, so it stays CI-green either way.
if python3 -c "import playwright, websockets" 2>/dev/null; then
    run_test "test_web_ui" python3 "$SCRIPT_DIR/test_web_ui.py"
    run_test "test_web_ui_chaos" python3 "$SCRIPT_DIR/test_web_ui_chaos.py"
elif [[ -n "${CI:-}" ]]; then
    run_test "test_web_ui" bash -c \
        'echo "ERROR: playwright/websockets not installed — required in CI"; exit 1'
    run_test "test_web_ui_chaos" bash -c \
        'echo "ERROR: playwright/websockets not installed — required in CI"; exit 1'
else
    skip_test "test_web_ui" "playwright or websockets not installed"
    skip_test "test_web_ui_chaos" "playwright or websockets not installed"
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

# Summary
echo ""
echo "════════════════════════════════════════"
echo "  SUMMARY"
echo "════════════════════════════════════════"
total=$((passed + failed + skipped))
executed=$((passed + failed))
echo "  Executed: $executed (passed $passed, failed $failed), skipped $skipped, total $total"
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
