#!/usr/bin/env bash
# parallel_runner.sh — the reusable core of scripts/check_parallel.sh: run N
# named shell commands concurrently, capped, with each one's output buffered
# to its own log and the results printed grouped + attributable at the end.
#
# Sourced (not executed) by:
#   - scripts/check_parallel.sh, with the real `make check` suites
#   - tests/test_check_parallel.sh, with synthetic jobs, to regression-test
#     the aggregation/crash-detection logic itself without paying for a real
#     Playwright/node/go run.
#
# Caller contract: set these BEFORE calling run_parallel_jobs (no args, no
# return value other than the exit status):
#   JOB_NAMES   — bash array of job names (used as log/exit filenames)
#   JOB_CMDS    — parallel bash array of shell command strings (run via
#                 `bash -c`, so normal quoting/operators/&&/globs all work)
#   CAP         — max concurrent jobs (>=1)
#   RESULTS_DIR — directory for per-job .log / .exit files (recreated fresh)
#
# Returns 0 if every job exited 0; 1 if any job failed OR crashed (its own
# shell died before it could report an exit status, e.g. OOM-killed —
# reported as "SUITE CRASHED", never silently treated as a pass).
set -uo pipefail

run_parallel_jobs() {
    rm -rf "$RESULTS_DIR"
    mkdir -p "$RESULTS_DIR"

    # Counting semaphore via a named pipe — portable to bash 3.2 (macOS's
    # /bin/bash default), unlike `wait -n` (bash 4.3+ only).
    local sem_fifo
    sem_fifo=$(mktemp -u "${TMPDIR:-/tmp}/pgwt-check-sem.XXXXXX")
    mkfifo "$sem_fifo"
    exec 8<>"$sem_fifo"
    rm -f "$sem_fifo"
    local i
    for ((i = 0; i < CAP; i++)); do printf '\n' >&8; done

    local pids=()
    for i in "${!JOB_NAMES[@]}"; do
        local name="${JOB_NAMES[$i]}"
        local cmd="${JOB_CMDS[$i]}"
        read -r -u 8 _   # take a slot; blocks once CAP jobs are in flight
        (
            local log="$RESULTS_DIR/$name.log"
            : > "$log"
            bash -c "$cmd" >"$log" 2>&1
            local ec=$?
            # Written unconditionally (even if $cmd crashed/was killed) so a
            # missing sentinel below can only mean THIS wrapper never got
            # this far (its own shell died first).
            echo "$ec" > "$RESULTS_DIR/$name.exit"
            printf '\n' >&8  # release the slot
        ) &
        pids+=($!)
    done

    for pid in "${pids[@]}"; do
        wait "$pid"
    done
    exec 8>&-

    local overall=0
    for name in "${JOB_NAMES[@]}"; do
        printf '\n\033[1m--- suite: %s ---\033[0m\n' "$name"
        local log="$RESULTS_DIR/$name.log"
        [[ -f "$log" ]] && cat "$log"
        local exit_file="$RESULTS_DIR/$name.exit"
        if [[ ! -f "$exit_file" ]]; then
            echo "SUITE CRASHED: $name (no exit status recorded — the job's" \
                 "own shell died, e.g. OOM-killed, before it could report)"
            overall=1
            continue
        fi
        local ec
        ec=$(cat "$exit_file")
        if [[ "$ec" == "0" ]]; then
            echo "SUITE PASSED: $name"
        else
            echo "SUITE FAILED: $name (exit $ec)"
            overall=1
        fi
    done

    echo
    if [[ $overall -ne 0 ]]; then
        echo "PARALLEL CHECK: FAIL"
    else
        echo "PARALLEL CHECK: PASS (${#JOB_NAMES[@]} suites, cap=$CAP)"
    fi
    return $overall
}
