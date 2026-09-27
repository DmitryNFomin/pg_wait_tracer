#!/bin/bash
# observer_bias_study.sh — issue #115 / docs/OBSERVER_BIAS.md: the ONE command
# that re-runs the whole observer-bias comparison and prints a table.
#
# Runs three legs, each N interleaved saturated(8 clients)/unsaturated(2
# clients) captures against the same pgbench workload:
#   reconcile  our exact tier, our sampled tier, pg_stat_activity, the
#              kernel arbiter (schedstat counters + sched tracepoints),
#              and the sampler's own directly-measured scheduling bias
#   pgws       adds pg_wait_sampling and its collector's scheduling bias,
#              scored with the SAME instrument as the pg_stat_activity poller
#   xtab       the exact-tier-vs-kernel gap decomposed by intersecting the
#              two timelines nanosecond by nanosecond (closes by construction)
#
# Preconditions (this is the live tier: Linux, root, a running PostgreSQL
# with pgbench initialised and pg_wait_sampling installed, bpftrace) --
# exactly what `make box-check` / an ephemeral VM from the gate snapshot
# already provides. Nothing here runs on macOS (see CLAUDE.md).
#
#   sudo sysctl -w kernel.sched_schedstats=1     (asserted below, not assumed)
#   createdb postgres owner postgres; pgbench -i -s 10 postgres
#   CREATE EXTENSION pg_wait_sampling;           (in postgresql.conf's
#                                                  shared_preload_libraries)
#
# Usage:
#   sudo tests/observer_bias_study.sh [REPEATS]        # REPEATS default 3
#   REPEATS=1 sudo tests/observer_bias_study.sh         # quick smoke
#   LEGS="reconcile xtab" sudo tests/observer_bias_study.sh   # skip pgws
#
# Expected shape (docs/OBSERVER_BIAS.md has the full numbers): the kernel
# arbiter reports the highest on-or-runnable share in every regime; our
# exact tier is closest to it; the sampled tier, pg_stat_activity and
# pg_wait_sampling under-report by increasing amounts, worst saturated.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$HERE/.."
N=${1:-${REPEATS:-3}}
LEGS=${LEGS:-"reconcile pgws xtab"}
OUTBASE=${OUTBASE:-/root/pgwt-observer-bias-study}

fail() { echo "observer_bias_study: FATAL: $*" >&2; exit 1; }

[[ "$(uname -s)" == "Linux" ]] || fail "Linux only (see CLAUDE.md: nothing eBPF-related runs on macOS)"
[[ $EUID -eq 0 ]] || fail "must run as root (bpftrace needs CAP_BPF/CAP_PERFMON)"
command -v bpftrace >/dev/null || fail "bpftrace not found"
command -v pgbench >/dev/null || fail "pgbench not found"
command -v psql >/dev/null || fail "psql not found"

# The precondition every capture script and kernel_cpu_arbiter.py's own
# `sample` subcommand also assert on their own: without this, run_delay
# silently reads 0 and every run-queue-delay number in the study is wrong
# by construction, not measured. Checked here too so a bad machine is
# rejected in one second, not after the first ~1-minute capture completes.
SCHEDSTATS=$(cat /proc/sys/kernel/sched_schedstats 2>/dev/null || echo "")
[[ "$SCHEDSTATS" == "1" ]] || fail \
    "kernel.sched_schedstats=$SCHEDSTATS -- run 'sysctl -w kernel.sched_schedstats=1' first (every schedstat-derived run-queue-delay number would otherwise silently read 0, not fail)"

echo "=== building pg_wait_tracer + tests/xp_gate ==="
make -C "$REPO" -q >/dev/null 2>&1 || make -C "$REPO" || fail "project build failed"
make -C "$REPO/tests" xp_gate || fail "tests/xp_gate build failed"
[[ -x "$REPO/tests/xp_gate" ]] || fail "tests/xp_gate did not build"

mkdir -p "$OUTBASE"
declare -A RUN_SCRIPT=(
    [reconcile]="$HERE/results/issue115_reconcile/recon_run.sh"
    [pgws]="$HERE/results/issue115_pgws/pgws_run.sh"
    [xtab]="$HERE/results/issue115_xtab/xtab_run.sh"
)
declare -A SUMMARY_SCRIPT=(
    [reconcile]="$HERE/results/issue115_reconcile/recon_summary.py"
    [pgws]="$HERE/results/issue115_pgws/pgws_summary.py"
    [xtab]="$HERE/results/issue115_xtab/xtab_summary.py"
)
declare -A OUT_DIR=(
    [reconcile]="$OUTBASE/reconcile"
    [pgws]="$OUTBASE/pgws"
    [xtab]="$OUTBASE/xtab"
)

fail_legs=0
for leg in $LEGS; do
    script=${RUN_SCRIPT[$leg]:-}
    [[ -n "$script" ]] || fail "unknown leg '$leg' (want a subset of: reconcile pgws xtab)"
    echo
    echo "=== leg: $leg ($N repeats x sat/unsat -> ${OUT_DIR[$leg]}) ==="
    if ! OUTBASE="${OUT_DIR[$leg]}" bash "$script" "$N"; then
        echo "observer_bias_study: leg '$leg' FAILED (see output above)" >&2
        fail_legs=1
    fi
done

echo
echo "############################################################"
echo "# TABLE (mean [min..max] over $N repeats per regime)"
echo "############################################################"
for leg in $LEGS; do
    echo
    echo "--- $leg: ${OUT_DIR[$leg]} ---"
    python3 "${SUMMARY_SCRIPT[$leg]}" "${OUT_DIR[$leg]}" || echo "(summary failed for $leg -- inspect ${OUT_DIR[$leg]}/*.txt directly)"
done

[[ $fail_legs -eq 0 ]] || { echo; echo "observer_bias_study: one or more legs FAILED"; exit 1; }
echo
echo "observer_bias_study: DONE"
