#!/bin/bash
# testutil.sh — Shared test helpers for pg_wait_tracer tests
#
# Source this file in shell tests:
#   source "$(dirname "$0")/testutil.sh"
#
# Usage:
#   find_postmaster                   # highest PG version available
#   find_postmaster --pg-version 16   # specific PG version

# postmaster_version PID
# Resolves PID's PostgreSQL major version. Tries the Debian/Ubuntu PGDG
# layout's binary path first (/usr/lib/postgresql/<N>/bin/postgres); falls
# back to `postgres --version` so non-Debian layouts (RPM: /usr/pgsql-<N>/
# bin/postgres, or anything else non-standard) still resolve. Outputs the
# version to stdout. Returns 1 (no output) if it can't be determined.
postmaster_version() {
    local pid="$1"
    local exe
    exe=$(readlink "/proc/$pid/exe" 2>/dev/null) || return 1
    local ver
    ver=$(echo "$exe" | grep -oP 'postgresql/\K\d+(?=/)')
    if [[ -z "$ver" ]]; then
        ver=$("$exe" --version 2>/dev/null | grep -oP 'PostgreSQL\)?\s+\K\d+')
    fi
    [[ -z "$ver" ]] && return 1
    echo "$ver"
    return 0
}

# find_postmaster [--pg-version N]
# Finds the postmaster PID. Returns 0 on success, 1 on failure.
# Outputs the PID to stdout.
find_postmaster() {
    local target_version=""
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --pg-version) target_version="$2"; shift 2 ;;
            *) shift ;;
        esac
    done

    local best_pid=""
    local best_ver=0

    for pid in $(pgrep -x postgres 2>/dev/null); do
        # Skip children — a postmaster's parent is not postgres
        local ppid
        ppid=$(awk '/^PPid:/ {print $2}' /proc/$pid/status 2>/dev/null) || continue
        local parent_comm
        parent_comm=$(cat /proc/$ppid/comm 2>/dev/null)
        [[ "$parent_comm" == "postgres" ]] && continue

        local ver
        ver=$(postmaster_version "$pid") || continue

        if [[ -n "$target_version" ]]; then
            if [[ "$ver" == "$target_version" ]]; then
                echo "$pid"
                return 0
            fi
        else
            if [[ "$ver" -gt "$best_ver" ]]; then
                best_ver=$ver
                best_pid=$pid
            fi
        fi
    done

    if [[ -n "$best_pid" ]]; then
        echo "$best_pid"
        return 0
    fi
    return 1
}
