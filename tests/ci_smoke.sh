#!/bin/bash
# ci_smoke.sh — real-PostgreSQL capture smoke test wrapper (Phase T0: TST-1/2).
#
# The CI capture-smoke job calls this against a freshly-installed PGDG
# PostgreSQL; it also runs unchanged on any root box with a running PG.
# It drives tests/test_capture_smoke.py in BOTH capture modes plus the
# existing full-mode deterministic-accuracy test, and fails if capture
# records nothing (that is its entire purpose).
#
#   tiered (the shipped default) — HARD requirement, always asserted:
#     sampler capture, live-view feed (PR #30), query attribution (PR #31),
#     trace-file write + pgwt-server read-back.
#   full (hardware watchpoints) — depends on the environment: perf hw
#     breakpoints may be unavailable on some hypervisors. The probe below
#     DISCOVERS this empirically. If watchpoints work, the full-mode
#     assertions are just as hard; if they don't, the full-mode section is
#     skipped LOUDLY (stdout + $GITHUB_STEP_SUMMARY) — never silently.
#
# Usage: sudo tests/ci_smoke.sh [--pid POSTMASTER_PID] [--pg-version N]
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
source "$SCRIPT_DIR/testutil.sh"

PM_PID=""
PG_VERSION=""
EXPECT_UNSUPPORTED=0
# --core: cross-distro CONTAINER mode (the nightly matrix). Proves the capture
# path works on each distro — the daemon attaches, records real wait events, and
# attributes query ids, live AND to a trace file — and LOUDLY skips the sections
# that need hardware watchpoints to actually FIRE or precise multi-core timing:
# full-mode exactness, deterministic accuracy, the state_map-full loudness, and
# the CPU-storm AAS/escalation. Those are gated on the T0 hosted runner and the
# live EL8/EL9 boxes (run_all.sh --require-live), where watchpoints fire and the
# scheduler is not a nested hypervisor. Default (no --core) is unchanged: the
# full battery, exactly as the T0 CI job runs it.
CORE=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --pid) PM_PID="$2"; shift 2 ;;
        --pg-version) PG_VERSION="$2"; shift 2 ;;
        --expect-unsupported) EXPECT_UNSUPPORTED=1; shift ;;
        --core) CORE=1; shift ;;
        *) echo "Usage: $0 [--pid POSTMASTER_PID] [--pg-version N] [--expect-unsupported] [--core]"; exit 1 ;;
    esac
done

if [[ $(id -u) -ne 0 ]]; then
    echo "ERROR: must run as root (sudo)"; exit 1
fi
if [[ ! -x "$PROJECT_DIR/pg_wait_tracer" || ! -x "$PROJECT_DIR/pgwt-server" ]]; then
    echo "ERROR: build first (make)"; exit 1
fi

if [[ -z "$PM_PID" ]]; then
    if [[ -n "$PG_VERSION" ]]; then
        PM_PID=$(find_postmaster --pg-version "$PG_VERSION")
    else
        PM_PID=$(find_postmaster)
    fi
fi
if [[ -z "$PM_PID" ]]; then
    echo "ERROR: cannot find postmaster PID"; exit 1
fi
echo "=== ci_smoke (postmaster PID $PM_PID, pg-version ${PG_VERSION:-auto}) ==="

summary_early() {
    echo "$1"
    [[ -n "${GITHUB_STEP_SUMMARY:-}" ]] && echo "$1" >> "$GITHUB_STEP_SUMMARY" || true
}

# ── Unsupported-version cell (e.g. PG16 until Track D lands) ───────
# The contract under test is fail-safe refusal: no known
# offsetof(PGPROC, wait_event_info) for this version => the tracer must
# exit non-zero with a loud FATAL, never attach and trace garbage (the
# CAP-2/CAP-3 "refuse on unknown layout" guarantee).
if [[ $EXPECT_UNSUPPORTED -eq 1 ]]; then
    echo "=== unsupported-version cell: tracer must refuse loudly ==="
    OUT=$("$PROJECT_DIR/pg_wait_tracer" --mode tiered --pid "$PM_PID" \
          --duration 5 --quiet 2>&1)
    RC=$?
    echo "$OUT" | tail -5
    if [[ $RC -ne 0 ]] && grep -q "no known offsetof(PGPROC, wait_event_info)" <<<"$OUT"; then
        summary_early "- PASS: unsupported PG version refused loudly (rc=$RC, fail-safe contract)"
        exit 0
    fi
    summary_early "- **FAIL**: expected a loud refusal on an unsupported PG version (rc=$RC)"
    exit 1
fi

summary() {
    echo "$1"
    if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
        echo "$1" >> "$GITHUB_STEP_SUMMARY"
    fi
}

# ── Watchpoint availability probe ──────────────────────────────────
# perf hardware breakpoints (PERF_TYPE_BREAKPOINT) need debug-register
# support from the hypervisor; discover rather than assume.
probe_watchpoints() {
    local src bin
    src=$(mktemp /tmp/pgwt_wp_probe_XXXXXX.c)
    bin="${src%.c}"
    cat > "$src" <<'EOF'
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <sys/syscall.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
static volatile int target;
int main(void) {
    struct perf_event_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.type = PERF_TYPE_BREAKPOINT;
    attr.size = sizeof(attr);
    attr.bp_type = HW_BREAKPOINT_W;
    attr.bp_addr = (unsigned long)&target;
    attr.bp_len = HW_BREAKPOINT_LEN_4;
    attr.sample_period = 1;
    attr.wakeup_events = 1;
    int fd = syscall(SYS_perf_event_open, &attr, 0, -1, -1, 0);
    if (fd < 0) { fprintf(stderr, "perf_event_open: %s\n", strerror(errno)); return 1; }
    close(fd);
    return 0;
}
EOF
    if ! cc -o "$bin" "$src" 2>/dev/null; then
        rm -f "$src" "$bin"
        echo "unknown"; return
    fi
    if "$bin" 2>/tmp/pgwt_wp_probe.err; then
        echo "yes"
    else
        echo "no"
    fi
    rm -f "$src" "$bin"
}

WP=$(probe_watchpoints)
echo "Watchpoint probe: $WP"
[[ "$WP" == "no" ]] && cat /tmp/pgwt_wp_probe.err 2>/dev/null

overall=0

run_section() {
    local name="$1"; shift
    echo ""
    echo "════════════════════════════════════════"
    echo "  $name"
    echo "════════════════════════════════════════"
    if "$@"; then
        summary "- PASS: $name"
    else
        summary "- **FAIL**: $name"
        overall=1
    fi
}

# ── Tiered mode: the shipped default — always a hard requirement ───
# --core (container matrix) relaxes the exact-duration magnitude to
# presence-only, since a nested container's watchpoints do not fire.
run_section "capture smoke: --mode tiered (live view + trace file)" \
    python3 "$SCRIPT_DIR/test_capture_smoke.py" --mode tiered --pid "$PM_PID" \
        $([[ $CORE -eq 1 ]] && echo --capture-core)

# ── Stages 3/4: tiered query/activity probes are escalation-only. ──
# Validated PG13-18 must have zero sampled firings, attach both links atomically
# for repeated exact generations, and detach without stale preseeds. A degraded
# layout retains the legacy pinned pair. This also covers a mid-query straddler,
# backend fork/exit during a window, and partial-attach rollback.
run_section "tiered exact-probe lifecycle (Stages 3/4)" \
    python3 "$SCRIPT_DIR/test_uprobe_fired.py" --pid "$PM_PID" \
        ${PG_VERSION:+--pg-version "$PG_VERSION"}

if [[ $CORE -eq 1 ]]; then
    # Container matrix: the daemon attaches, records real attributed events
    # live + to disk (asserted above), and its exact-probe lifecycle works on
    # binary layout. The watchpoint-fidelity / precise-scheduling sections are
    # skipped LOUDLY — they are gated on the T0 hosted runner and the live
    # EL8/EL9 boxes.
    summary "### NOTE (--core, container matrix): watchpoint-fidelity sections SKIPPED"
    summary "Skipped here: --mode full exactness, deterministic accuracy, the"
    summary "state_map-full loudness (CAP-1), and CPU-storm AAS/escalation."
    summary "They need hardware watchpoints to actually FIRE and precise"
    summary "multi-core scheduling, which a nested privileged container does"
    summary "not provide (the probe can OPEN a breakpoint that never traps)."
    summary "The core capture proof above — build, BPF attach, real attributed"
    summary "events live + on disk, exact-probe lifecycle — is the portable gate; the"
    summary "watchpoint sections run on the T0 hosted runner + live EL8/EL9"
    summary "boxes (run_all.sh --require-live)."
else
    # ── T4/CAP-1: a full state_map must be loud (metrics + ERROR log). ──
    # Uses the PGWT_STATE_MAP_ENTRIES test hook to shrink the map, so the
    # loud path is proven with ~10 connections instead of >1024.
    run_section "state_map full is loud (CAP-1, --mode tiered)" \
        python3 "$SCRIPT_DIR/test_state_map_loud.py" --pid "$PM_PID"

    # ── Full mode: hard when watchpoints exist; loud skip when they don't ──
    if [[ "$WP" == "yes" ]]; then
        run_section "capture smoke: --mode full (live view + trace file)" \
            python3 "$SCRIPT_DIR/test_capture_smoke.py" --mode full --pid "$PM_PID"
        run_section "deterministic accuracy: exact counts/durations (--mode full)" \
            python3 "$SCRIPT_DIR/test_deterministic.py" --pid "$PM_PID"
    else
        summary "### WARNING: full-mode watchpoint assertions SKIPPED"
        summary "perf hardware breakpoints are unavailable in this environment"
        summary "(probe: perf_event_open(PERF_TYPE_BREAKPOINT) failed — see log)."
        summary "Tiered/sampled assertions above remain the hard gate; full-mode"
        summary "exactness is validated on real hardware via tests/run_all.sh"
        summary "--require-live (Trust Milestone standing rule)."
    fi
fi

echo ""
if [[ $overall -eq 0 ]]; then
    echo "ci_smoke: ALL SECTIONS PASSED (watchpoints: $WP)"
else
    echo "ci_smoke: FAILURES (watchpoints: $WP)"
fi
exit $overall
