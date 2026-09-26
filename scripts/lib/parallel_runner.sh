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
# reported as "SUITE CRASHED", never silently treated as a pass), or if the
# job list/config itself is unusable (empty JOB_NAMES, JOB_NAMES/JOB_CMDS
# length mismatch, an empty command, a non-mkfifo-capable TMPDIR, or CAP not
# an integer >= 1) — refused loudly before any job runs, never silently
# under-covering the suite list (see the bypass-suite rule, issue #183).
set -uo pipefail

run_parallel_jobs() {
    # ── Refuse rather than silently under-cover (bypass-suite spirit, #183):
    # a gate that cannot see its own job list must refuse, never approve. Each
    # of these would otherwise let a suite never start yet never be reported
    # as failed either — invisible, not even a CRASH line — or deadlock every
    # job forever. Checked BEFORE anything runs, so a bad config never leaves
    # partial results behind to be misread as a real pass.
    if [[ ${#JOB_NAMES[@]} -eq 0 ]]; then
        echo "parallel_runner: JOB_NAMES is empty — refusing to report an empty run as a pass" >&2
        return 1
    fi
    if [[ ${#JOB_NAMES[@]} -ne ${#JOB_CMDS[@]} ]]; then
        echo "parallel_runner: JOB_NAMES has ${#JOB_NAMES[@]} entries but JOB_CMDS has" \
             "${#JOB_CMDS[@]} — a dropped name/command pair would never start and never be" \
             "reported as failed; refusing" >&2
        return 1
    fi
    local i
    for i in "${!JOB_NAMES[@]}"; do
        if [[ -z "${JOB_CMDS[$i]}" ]]; then
            echo "parallel_runner: job '${JOB_NAMES[$i]}' has an empty command —" \
                 "'bash -c \"\"' exits 0 and would report SUITE PASSED without checking" \
                 "anything; refusing" >&2
            return 1
        fi
    done
    if ! [[ "$CAP" =~ ^[0-9]+$ ]] || [[ "$CAP" -lt 1 ]]; then
        echo "parallel_runner: CAP='$CAP' must be an integer >= 1 (CAP=0 would deadlock" \
             "every job waiting forever for a semaphore slot that's never released)" >&2
        return 1
    fi

    rm -rf "$RESULTS_DIR"
    mkdir -p "$RESULTS_DIR"

    # Counting semaphore via a named pipe — portable to bash 3.2 (macOS's
    # /bin/bash default), unlike `wait -n` (bash 4.3+ only).
    local sem_fifo
    sem_fifo=$(mktemp -u "${TMPDIR:-/tmp}/pgwt-check-sem.XXXXXX")
    if ! mkfifo "$sem_fifo"; then
        echo "parallel_runner: mkfifo failed for $sem_fifo — cannot create the concurrency" \
             "semaphore (a plain file here would silently stop CAP from blocking anything);" \
             "refusing" >&2
        return 1
    fi
    exec 8<>"$sem_fifo"
    rm -f "$sem_fifo"
    for ((i = 0; i < CAP; i++)); do printf '\n' >&8; done

    echo "check-parallel: launching ${#JOB_NAMES[@]} jobs (cap=$CAP) — output is buffered and" \
         "printed grouped only once every job finishes; follow progress live with:" \
         "tail -f $RESULTS_DIR/*.log"

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
