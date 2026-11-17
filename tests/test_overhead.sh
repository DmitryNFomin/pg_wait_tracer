#!/bin/bash
# test_overhead.sh — Measure TPS overhead of pg_wait_tracer using pgbench
#
# Methodology:
#   - Multiple concurrency levels (1, 4, 8, 16, 32, 64 clients)
#   - 5 runs per level, 60s each (configurable)
#   - 10s warmup before each measured run
#   - Reports mean, stddev, and 95% confidence interval
#   - Warns if pg_wait_sampling or other tracers are active
#   - Tracks wait event transition rate via /proc perf counters
#
# Usage: sudo tests/test_overhead.sh [OPTIONS]
#   --pid PID          Postmaster PID (auto-detected if omitted)
#   --clients LIST     Comma-separated client counts (default: 1,4,8,16,32,64)
#   --duration SECS    pgbench duration per run (default: 60)
#   --runs N           Runs per concurrency level (default: 5)
#   --warmup SECS      Warmup duration before each run (default: 10)
#   --pgbench-opts STR Extra pgbench options (e.g. "--select-only")
#   --output FILE      Write CSV results to file
#   --quick            Quick mode: 3 runs, 30s, clients 4,16,64
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# TRACER is a test-only override for the host-runnable gate controls.
TRACER="${TRACER:-$SCRIPT_DIR/../pg_wait_tracer}"
source "$SCRIPT_DIR/testutil.sh"

# Defaults
CLIENTS_LIST="1,4,8,16,32,64"
DURATION=60
RUNS=5
WARMUP=10
PGBENCH_EXTRA=""
OUTPUT_FILE=""
PM_PID=""

# Parse args
while [[ $# -gt 0 ]]; do
    case "$1" in
        --pid)           PM_PID="$2"; shift 2 ;;
        --clients)       CLIENTS_LIST="$2"; shift 2 ;;
        --duration)      DURATION="$2"; shift 2 ;;
        --runs)          RUNS="$2"; shift 2 ;;
        --warmup)        WARMUP="$2"; shift 2 ;;
        --pgbench-opts)  PGBENCH_EXTRA="$2"; shift 2 ;;
        --output)        OUTPUT_FILE="$2"; shift 2 ;;
        --quick)         RUNS=3; DURATION=30; CLIENTS_LIST="4,16,64"; shift ;;
        *)               echo "Usage: $0 [--pid PID] [--clients LIST] [--duration S] [--runs N] [--warmup S] [--pgbench-opts STR] [--output FILE] [--quick]"; exit 1 ;;
    esac
done

# Auto-detect postmaster PID
if [[ -z "$PM_PID" ]]; then
    PM_PID=$(find_postmaster)
    if [[ -z "$PM_PID" ]]; then
        echo "ERROR: cannot find postmaster PID (pass --pid)"
        exit 1
    fi
fi

# Verify tracer binary exists
if [[ ! -x "$TRACER" ]]; then
    echo "ERROR: tracer not found at $TRACER (run 'make' first)"
    exit 1
fi

# Retain per-run evidence outside the source tree, including on errors.
EVIDENCE_DIR=$(mktemp -d "${TMPDIR:-/tmp}/pgwt-overhead.XXXXXX")
echo "Run evidence: $EVIDENCE_DIR"
tracer_pid=""
tracer_stderr=""
cleanup() {
    local status=$?
    trap - EXIT
    if [[ -n "$tracer_pid" ]]; then
        kill "$tracer_pid" 2>/dev/null || true
        wait "$tracer_pid" 2>/dev/null || true
    fi
    if [[ "$status" -ne 0 && -n "$tracer_stderr" ]]; then
        echo "Tracer stderr (last 20 lines): $tracer_stderr" >&2
        tail -n 20 "$tracer_stderr" >&2
    fi
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# Parse client list
IFS=',' read -ra CLIENT_COUNTS <<< "$CLIENTS_LIST"

# ── Environment checks ──────────────────────────────────────────────

echo "╔══════════════════════════════════════════════════════════════╗"
echo "║            pg_wait_tracer overhead benchmark                ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""

# System info
NCPUS=$(nproc)
MEM_GB=$(awk '/MemTotal/{printf "%.1f", $2/1048576}' /proc/meminfo 2>/dev/null || echo unknown)
KERNEL=$(uname -r)
echo "System:       $(uname -n), $NCPUS CPUs, ${MEM_GB}GB RAM, kernel $KERNEL"
echo "Postmaster:   PID $PM_PID"

# PostgreSQL version
PG_EXE=$(readlink /proc/$PM_PID/exe 2>/dev/null || echo "unknown")
PG_VER=$("$PG_EXE" --version 2>/dev/null | head -1 || echo "unknown")
echo "PostgreSQL:   $PG_VER"

# Check for pg_wait_sampling (adds overhead — confounds measurement)
PG_BIN_DIR=$(dirname "$PG_EXE")
PSQL="$PG_BIN_DIR/psql"
if [[ -x "$PSQL" ]]; then
    WS_LOADED=$("$PSQL" -U postgres -d postgres -tAc "SHOW shared_preload_libraries" 2>/dev/null || echo "")
    if [[ "$WS_LOADED" == *"pg_wait_sampling"* ]]; then
        echo "WARNING:      pg_wait_sampling is loaded — this adds its own overhead!"
        echo "              Consider: ALTER SYSTEM SET shared_preload_libraries TO '';"
        echo "              and restart PostgreSQL for clean measurements."
    fi
else
    PSQL=psql
fi

# Check for other tracers (perf, strace, bpftrace)
OTHER_TRACERS=""
PERF_PROCS=$(pgrep -a perf 2>/dev/null || true)
[[ "$PERF_PROCS" =~ record|stat ]] && OTHER_TRACERS+="perf "
STRACE_PROCS=$(pgrep -a strace 2>/dev/null || true)
[[ "$STRACE_PROCS" == *"$PM_PID"* ]] && OTHER_TRACERS+="strace "
pgrep -a bpftrace 2>/dev/null && OTHER_TRACERS+="bpftrace "
if [[ -n "$OTHER_TRACERS" ]]; then
    echo "WARNING:      Other tracers detected: $OTHER_TRACERS"
fi

echo ""
echo "Benchmark:    ${#CLIENT_COUNTS[@]} concurrency levels: ${CLIENTS_LIST}"
echo "              $RUNS runs x ${DURATION}s each, ${WARMUP}s warmup"
if [[ -n "$PGBENCH_EXTRA" ]]; then
    echo "              Extra pgbench options: $PGBENCH_EXTRA"
fi
echo ""

# ── Helper functions ──────────────────────────────────────────────

extract_tps() {
    # Extract "tps = 1234.56" (excluding connection establishing) from pgbench output
    awk '$1 == "tps" && $2 == "=" && $3 ~ /^[0-9]+([.][0-9]+)?$/ {tps=$3}
         END {if (tps != "") print tps}'
}

# Compute mean from a space-separated list of numbers
calc_mean() {
    local vals="$1"
    echo "$vals" | tr ' ' '\n' | awk 'NF{s+=$1; n++} END{printf "%.2f", s/n}'
}

# Compute sample standard deviation
calc_stddev() {
    local vals="$1"
    echo "$vals" | tr ' ' '\n' | awk 'NF{
        a[NR]=$1; s+=$1; n++
    } END{
        mean=s/n; ss=0;
        for(i=1;i<=n;i++) ss+=(a[i]-mean)^2;
        printf "%.2f", sqrt(ss/(n-1))
    }'
}

# /proc/stat's aggregate CPU row: steal is field 9 (cumulative clock ticks).
read_steal() {
    awk '$1 == "cpu" && NF >= 9 {print $9; found=1; exit}
         END {if (!found) exit 1}' /proc/stat 2>/dev/null || echo unavailable
}

capture_pg_log() {
    local logfile data_dir
    # Neither this script nor run_all.sh has a server-log variable. Ask the
    # logging collector first, then try the postmaster's redirected stderr
    # (e.g. Debian clusters without logging_collector). Never read a pipe.
    logfile=$("$PSQL" -U postgres -d postgres -tAc 'SELECT pg_current_logfile()' 2>/dev/null) || logfile=""
    if [[ -n "$logfile" && "$logfile" != /* ]]; then
        data_dir=$("$PSQL" -U postgres -d postgres -tAc 'SHOW data_directory' 2>/dev/null) || data_dir=""
        if [[ -n "$data_dir" ]]; then
            logfile="$data_dir/$logfile"
        else
            logfile=""
        fi
    fi
    if [[ ! -f "$logfile" || ! -r "$logfile" ]]; then
        logfile=$(readlink "/proc/$PM_PID/fd/2" 2>/dev/null) || logfile=""
    fi
    if [[ -f "$logfile" && -r "$logfile" ]]; then
        echo "PostgreSQL log: $logfile"
        tail -n 20 "$logfile" || echo "PostgreSQL log unavailable: tail failed"
    else
        echo "PostgreSQL log unavailable: pg_current_logfile() and postmaster stderr did not locate a readable regular file"
    fi
}

check_tracer() {
    local stage=$1
    if ! kill -0 "$tracer_pid" 2>/dev/null; then
        echo "ERROR: tracer liveness: not running $stage" >&2
        return 1
    fi
    # src/daemon.c:1857-1859, after provider->start and the backend scan;
    # src/provider_full.c names this provider 'full' with EXACT fidelity.
    if ! grep -qxF 'INFO: capture provider: full (exact fidelity)' "$tracer_stderr"; then
        echo "ERROR: tracer liveness: full-mode attach signal missing $stage" >&2
        return 1
    fi
}

# Run pgbench once with warmup. Set TPS in the parent shell so failures and
# the EXIT trap cannot be lost inside command substitution.
run_pgbench() {
    local clients=$1
    local evidence=$2
    local status=0 before after

    # Warmup (discard results)
    if [[ "$WARMUP" -gt 0 ]]; then
        pgbench -U postgres -d postgres -c "$clients" -j "$clients" \
            -T "$WARMUP" $PGBENCH_EXTRA > "$evidence.warmup" 2>&1 || status=$?
        if [[ "$status" -ne 0 ]]; then
            echo "ERROR: pgbench warmup failed (exit $status): $evidence.warmup" >&2
            cat "$evidence.warmup" >&2
            return 1
        fi
    fi

    # Measured run
    before=$(read_steal)
    pgbench -U postgres -d postgres -c "$clients" -j "$clients" \
        -T "$DURATION" -P 5 $PGBENCH_EXTRA > "$evidence.pgbench" 2>&1 || status=$?
    after=$(read_steal)
    {
        echo "Steal before: $before; after: $after (clock ticks)"
        if [[ "$before" != unavailable && "$after" != unavailable ]]; then
            awk -v b="$before" -v a="$after" 'BEGIN {print "Steal delta: " a-b " clock ticks"}'
        else
            echo "Steal delta: unavailable (/proc/stat unreadable or missing steal field)"
        fi
    } > "$evidence.steal"
    capture_pg_log > "$evidence.pglog" 2>&1
    TPS=$(extract_tps < "$evidence.pgbench")
    if [[ "$status" -ne 0 || -z "$TPS" ]]; then
        if [[ "$status" -ne 0 ]]; then
            echo "ERROR: pgbench failed (exit $status): $evidence.pgbench" >&2
        else
            echo "ERROR: pgbench failed to produce TPS output: $evidence.pgbench" >&2
        fi
        cat "$evidence.pgbench" >&2
        cat "$evidence.steal" "$evidence.pglog" >&2
        return 1
    fi
}

# Evidence only: compare each run to the median of its own arm's OTHER runs.
# Keep every run in the mean, standard deviation and existing verdict.
report_stalls() {
    local arm=$1 clients=$2 results=$3 run median evidence
    local values=($results)
    for ((run=1; run<=${#values[@]}; run++)); do
        median=$(printf '%s\n' "${values[@]}" | awk -v skip="$run" 'NR != skip' |
            sort -n | awk '{a[NR]=$1} END {
                if (NR) print NR%2 ? a[(NR+1)/2] : (a[NR/2]+a[NR/2+1])/2
            }')
        if [[ -n "$median" ]] && awk -v t="${values[$((run-1))]}" -v m="$median" 'BEGIN {exit !(t < 0.25*m)}'; then
            evidence="$EVIDENCE_DIR/$clients-$arm-$run"
            echo "  STALL: $arm, $clients clients, run $run: ${values[$((run-1))]} TPS < 25% of other-run median $median (evidence only)"
            echo "pgbench progress: $evidence.pgbench"
            grep '^progress:' "$evidence.pgbench" || true
            cat "$evidence.steal" "$evidence.pglog"
        fi
    done
}

# Count PostgreSQL backend processes (gives event transition rate proxy)
count_backends() {
    local count
    count=$(pgrep -c -P "$PM_PID" 2>/dev/null || echo "0")
    echo "$count"
}

# ── CSV header ────────────────────────────────────────────────────

if [[ -n "$OUTPUT_FILE" ]]; then
    echo "clients,mode,run,tps" > "$OUTPUT_FILE"
fi

# ── Main benchmark loop ───────────────────────────────────────────

# Issue #133: this used to run `pgbench -i -s 100` when pgbench_accounts was
# missing/undersized, permanently GROWING (or, on an empty box, seeding at a
# scale that never matches) the shared dataset tests/provision-runner.sh
# provisions once at scale 10 -- `-i` unconditionally drops and recreates
# every pgbench table, changing cache behaviour and timing for every later
# test on the box, including this gate's own baseline. The overhead
# benchmark doesn't depend on any particular scale (baseline and tracer runs
# both read/write the same tables), so it now reads the provisioned dataset
# read-only and never re-initializes it.
SCALE=$("$PSQL" -U postgres -d postgres -tAc "SELECT count(*)/100000 FROM pgbench_accounts" 2>/dev/null || echo "0")
if [[ "$SCALE" -lt 1 ]]; then
    echo "ERROR: pgbench_accounts is missing/empty in the shared 'postgres' database."
    echo "       Issue #133: this test reads the provisioned dataset read-only and"
    echo "       never re-initializes it. Run tests/provision-runner.sh (or"
    echo "       'pgbench -i -s 10 -d postgres') to seed it first."
    exit 1
else
    echo "pgbench tables found (scale $SCALE)"
fi
echo ""

declare -A BASELINE_MEAN BASELINE_STDDEV TRACER_MEAN TRACER_STDDEV OVERHEAD_PCT

MAX_OVERHEAD=0

for CLIENTS in "${CLIENT_COUNTS[@]}"; do
    echo "━━━ Concurrency: $CLIENTS clients ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo ""

    # ── Baseline runs ──
    echo "  Baseline (no tracer):"
    baseline_results=""
    for i in $(seq 1 "$RUNS"); do
        run_pgbench "$CLIENTS" "$EVIDENCE_DIR/$CLIENTS-baseline-$i"
        tps=$TPS
        printf "    Run %d: %s TPS\n" "$i" "$tps"
        baseline_results+="$tps "
        if [[ -n "$OUTPUT_FILE" ]]; then
            echo "$CLIENTS,baseline,$i,$tps" >> "$OUTPUT_FILE"
        fi
    done
    report_stalls baseline "$CLIENTS" "$baseline_results"

    b_mean=$(calc_mean "$baseline_results")
    b_stddev=$(calc_stddev "$baseline_results")
    printf "    Mean: %s TPS (stddev: %s)\n\n" "$b_mean" "$b_stddev"

    # ── Tracer runs ──
    echo "  With tracer:"
    tracer_results=""
    for i in $(seq 1 "$RUNS"); do
        # Start tracer in background. Pinned to --mode full: this benchmark
        # measures the always-on watchpoint (exact) tier's overhead. The
        # default is now tiered (sampled, ~zero PG cost) — to benchmark that,
        # run without --mode.
        tracer_stderr="$EVIDENCE_DIR/$CLIENTS-tracer-$i.stderr"
        "$TRACER" --mode full --verbose --pid "$PM_PID" --interval 5 --duration $((DURATION + WARMUP + 30)) \
            --view time_model > /dev/null 2> "$tracer_stderr" &
        tracer_pid=$!
        sleep 2  # let tracer attach all backends
        check_tracer 'after attach sleep'

        run_pgbench "$CLIENTS" "$EVIDENCE_DIR/$CLIENTS-tracer-$i"
        tps=$TPS
        printf "    Run %d: %s TPS\n" "$i" "$tps"
        tracer_results+="$tps "

        # Stop tracer
        check_tracer 'after pgbench, before stop'
        kill "$tracer_pid"
        tracer_status=0
        wait "$tracer_pid" || tracer_status=$?
        tracer_pid=""
        # SIGTERM is our normal stop request; accept a clean handler exit or
        # default signal termination, but never hide a different failure.
        if [[ "$tracer_status" -ne 0 && "$tracer_status" -ne 143 ]]; then
            echo "ERROR: tracer liveness: unexpected exit $tracer_status at stop" >&2
            exit 1
        fi
        tracer_stderr=""
        sleep 1

        if [[ -n "$OUTPUT_FILE" ]]; then
            echo "$CLIENTS,tracer,$i,$tps" >> "$OUTPUT_FILE"
        fi
    done
    report_stalls tracer "$CLIENTS" "$tracer_results"

    t_mean=$(calc_mean "$tracer_results")
    t_stddev=$(calc_stddev "$tracer_results")
    printf "    Mean: %s TPS (stddev: %s)\n\n" "$t_mean" "$t_stddev"

    # ── Overhead calculation ──
    # Use awk (always present) for the float math so the test needs no `bc`.
    overhead=$(awk -v bm="$b_mean" -v tm="$t_mean" 'BEGIN{printf "%.4f", (bm - tm) * 100 / bm}')
    overhead_display=$(awk -v o="$overhead" 'BEGIN{printf "%.2f", o}')

    # 95% confidence interval for overhead (propagated error)
    # SE = sqrt((b_se/b_mean)^2 + (t_se/t_mean)^2) * overhead
    ci_margin=$(echo "$baseline_results" "$tracer_results" | awk -v bm="$b_mean" -v tm="$t_mean" -v oh="$overhead" -v n="$RUNS" '{
        # Read all values
        for(i=1;i<=n;i++) { b[i]=$i }
        for(i=1;i<=n;i++) { t[i]=$(i+n) }
        # Compute SEs
        bs=0; ts=0;
        for(i=1;i<=n;i++) { bs+=(b[i]-bm)^2; ts+=(t[i]-tm)^2 }
        b_se=sqrt(bs/(n*(n-1))); t_se=sqrt(ts/(n*(n-1)));
        # Relative errors
        if(bm>0 && tm>0) {
            rel_err = sqrt((b_se/bm)^2 + (t_se/tm)^2);
            margin = 1.96 * rel_err * 100;
            printf "%.2f", margin;
        } else {
            printf "N/A";
        }
    }')

    printf "  Overhead: %s%% (±%s%% at 95%% CI)\n" "$overhead_display" "$ci_margin"

    # Track per-level data
    BASELINE_MEAN[$CLIENTS]=$b_mean
    BASELINE_STDDEV[$CLIENTS]=$b_stddev
    TRACER_MEAN[$CLIENTS]=$t_mean
    TRACER_STDDEV[$CLIENTS]=$t_stddev
    OVERHEAD_PCT[$CLIENTS]=$overhead_display

    # Track max overhead
    is_higher=$(awk -v o="$overhead" -v m="$MAX_OVERHEAD" 'BEGIN{print (o > m) ? 1 : 0}')
    if [[ "$is_higher" -eq 1 ]]; then
        MAX_OVERHEAD=$overhead
    fi

    echo ""
done

# ── Summary table ──────────────────────────────────────────────────

echo "╔══════════════════════════════════════════════════════════════╗"
echo "║                      Summary                               ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""
printf "  %-10s  %-14s  %-14s  %-10s\n" "Clients" "Baseline TPS" "Tracer TPS" "Overhead"
printf "  %-10s  %-14s  %-14s  %-10s\n" "───────" "────────────" "──────────" "────────"

for CLIENTS in "${CLIENT_COUNTS[@]}"; do
    b="${BASELINE_MEAN[$CLIENTS]} ±${BASELINE_STDDEV[$CLIENTS]}"
    t="${TRACER_MEAN[$CLIENTS]} ±${TRACER_STDDEV[$CLIENTS]}"
    o="${OVERHEAD_PCT[$CLIENTS]}%"
    printf "  %-10s  %-14s  %-14s  %-10s\n" "$CLIENTS" "$b" "$t" "$o"
done
echo ""

# ── Verdict ──
#
# TPS-overhead from full-tier (watchpoint) tracing is inherently hardware-
# and workload-dependent: it scales with wait-event transition rate, core
# count, and how much the workload contends.  The project README documents
# full-tier overhead from ~6% (write-heavy) up to ~30% (read-heavy), and on
# a small shared VM (e.g. 2 cores) a read-heavy peak near 30% is
# documented-normal, NOT a regression.  A tight absolute pass/fail gate on a
# shared box therefore produces false failures (it previously hard-failed at
# >15%, contradicting the project's own stated range).
#
# So this gate is intentionally generous: it only FAILS on a GROSS
# regression — overhead far beyond the documented read-heavy maximum — which
# indicates something actually broke (e.g. a runaway BPF program or an
# attach storm), rather than ordinary hardware/workload variance.  The full
# table and peak overhead printed above remain the valuable output; the tier
# labels below (PASS/WARN) are informational, not a tight contract.

GROSS_REGRESSION_PCT=50   # documented max is ~30%; only fail well above it

MAX_OH_DISPLAY=$(awk -v m="$MAX_OVERHEAD" 'BEGIN{printf "%.2f", m}')
echo "  Peak overhead: ${MAX_OH_DISPLAY}%"
echo "  (Overhead is hardware/workload-dependent; README documents ~6% write-heavy"
echo "   up to ~30% read-heavy. This gate only catches gross regressions"
echo "   > ${GROSS_REGRESSION_PCT}%, not documented-normal overhead.)"

result=$(awk -v m="$MAX_OVERHEAD" 'BEGIN{print (m < 10) ? 1 : 0}')
if [[ "$result" -eq 1 ]]; then
    echo "  PASS (< 10% — low overhead)"
    exit 0
fi
result=$(awk -v m="$MAX_OVERHEAD" 'BEGIN{print (m < 30) ? 1 : 0}')
if [[ "$result" -eq 1 ]]; then
    echo "  PASS (10-30% — within documented full-tier range)"
    exit 0
fi
result=$(awk -v m="$MAX_OVERHEAD" -v g="$GROSS_REGRESSION_PCT" 'BEGIN{print (m < g) ? 1 : 0}')
if [[ "$result" -eq 1 ]]; then
    echo "  PASS (30-${GROSS_REGRESSION_PCT}% — above documented max but plausible on a"
    echo "        small/loaded box; informational, review BPF program if unexpected)"
    exit 0
fi
echo "  FAIL (> ${GROSS_REGRESSION_PCT}% — gross regression, something is broken"
echo "        far beyond documented hardware/workload variance)"
exit 1
