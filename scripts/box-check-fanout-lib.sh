#!/usr/bin/env bash
# box-check-fanout-lib.sh — pure (no network, no VM, no side effects)
# helpers for scripts/box-check.sh's FANOUT=1 mode. Sourced by box-check.sh
# itself and, separately, by tests/test_box_check_fanout.sh so the
# PG-version-list parsing and the combined-summary formatting can be tested
# without ever touching Hetzner (same "pure builder" split CLAUDE.md asks
# for in the UI: the orchestration script stays thin, the logic that can be
# wrong lives in functions a test can call directly).
#
# Every function here is side-effect-free: it only reads its arguments and
# writes to stdout (or, for fanout_overall_result, only its own return
# code). None of them touch the network, the filesystem, or global state.

# fanout_default_versions
# The PostgreSQL majors FANOUT=1 fans out over when PG= is not given.
# Mirrors tests/provision-runner.sh's own `for V in 13 16 17 18` and
# .github/workflows/ci.yml's capture-smoke matrix — this is the same set,
# not a new one invented for fanout.
fanout_default_versions() {
    echo "13 16 17 18"
}

# fanout_parse_versions "$PG"
# $PG is the box-check.sh PG= value: empty (fan out over the default set),
# a single major ("13"), or a comma-separated list ("13,16,17,18"). Prints
# the resulting version list space-separated, one call = one list. Blank
# entries from stray commas/whitespace ("13,,17", " 13 , 17 ") are dropped
# rather than silently turned into an empty machine name downstream.
fanout_parse_versions() {
    local raw="${1:-}"
    if [[ -z "$raw" ]]; then
        fanout_default_versions
        return 0
    fi
    local out=()
    local tok
    IFS=',' read -ra parts <<<"$raw"
    for tok in "${parts[@]}"; do
        tok="${tok// /}"
        [[ -n "$tok" ]] && out+=("$tok")
    done
    echo "${out[@]}"
}

# fanout_status_line VERSION RC
# One attribution line for the combined summary, e.g.:
#   PG13: PASS (exit=0)
#   PG17: FAIL (exit=1)
fanout_status_line() {
    local version="$1" rc="$2"
    if [[ "$rc" -eq 0 ]]; then
        echo "PG$version: PASS (exit=$rc)"
    else
        echo "PG$version: FAIL (exit=$rc)"
    fi
}

# fanout_overall_result RC...
# Prints PASS/FAIL for the whole fanout run and returns 0/1 to match: ANY
# non-zero worker exit code fails the entire run (property #1 in the
# fanout issue: "fails the whole run if any machine fails"). No workers at
# all (empty argument list) is also a failure — an all-skip fanout is the
# same "refuse to report success" rule tests/run_all.sh already applies to
# an all-skip run, not a vacuous PASS.
fanout_overall_result() {
    if [[ $# -eq 0 ]]; then
        echo "FAIL"
        return 1
    fi
    local rc
    for rc in "$@"; do
        if [[ "$rc" -ne 0 ]]; then
            echo "FAIL"
            return 1
        fi
    done
    echo "PASS"
    return 0
}
