#!/bin/bash
# live_daemon_lib.sh -- shared helpers for driving a real daemon + real Go
# bridge session against a real PostgreSQL. Sourced (never executed
# directly) by tests/ui_live_smoke.sh (issue #93, ~30 min) and
# tests/demo_rehearsal.sh (issue #157, 30-45 min) so the two scripts share
# one PGPORT-derivation / process-teardown / readiness-polling
# implementation instead of drifting copies (CLAUDE.md "Extend, do not
# fork"). Every function is a plain bash function; nothing here forks a
# background process itself except the readiness probe, so `bash -n` plus
# careful review is what verifies it on the Mac -- like both callers, it
# only actually RUNS on Linux with root + a live PostgreSQL.

# stop_pid PID [BUDGET_S] [SIGNAL]
# Bounded wait: signal, then poll for exit, then KILL if it outlives the
# budget. Mirrors test_capture_smoke.py's terminate_and_wait (TERM + timeout
# + KILL) -- `wait` has no native timeout in bash. sig defaults to TERM.
stop_pid() {
    local pid="$1" budget_s="${2:-10}" sig="${3:-TERM}"
    [[ -z "$pid" ]] && return 0
    kill -0 "$pid" 2>/dev/null || return 0
    kill "-$sig" "$pid" 2>/dev/null
    local steps=$((budget_s * 2))
    local n=0
    while kill -0 "$pid" 2>/dev/null && [[ $n -lt $steps ]]; do
        sleep 0.5
        n=$((n + 1))
    done
    if kill -0 "$pid" 2>/dev/null; then
        kill -KILL "$pid" 2>/dev/null
    fi
    wait "$pid" 2>/dev/null
    return 0
}

# url_ready URL
# GET a URL, exit 0 iff it answers 200. Python (not curl) to avoid adding a
# new external-tool dependency on top of what the rest of the suite needs.
url_ready() {
    python3 - "$1" <<'PYEOF'
import sys, urllib.request
try:
    with urllib.request.urlopen(sys.argv[1], timeout=2) as r:
        sys.exit(0 if r.status == 200 else 1)
except Exception:
    sys.exit(1)
PYEOF
}

# derive_pgport PM_PID [PG_MAJOR] [LABEL]
# Sets and exports PGPORT for postmaster PM_PID. Three tiers, in order:
#   1. PGWT_PGPORT: explicit caller override, always wins.
#   2. Line 4 of the RESOLVED PM_PID's own postmaster.pid, via
#      /proc/$PM_PID/cwd (postmaster's CWD is its data directory) --
#      authoritative for any layout.
#   3. PGWT_PG_PORT_BASE + PG_MAJOR (derived from the exe path if not
#      given), only if postmaster.pid can't be read.
# NEVER trust an ambient/inherited PGPORT -- the gate box sets one in
# /etc/environment, which silently overrode an explicit --pg-version once
# and ran a whole walk against the wrong cluster (an 11/11 false failure).
# Exits 1 (via the caller's own `set -uo pipefail`) if PGPORT cannot be
# derived at all. LABEL prefixes every echo line so a shared log stays
# attributable to the right caller.
derive_pgport() {
    local pm_pid="$1" pg_major="${2:-}" label="${3:-live_daemon}"
    local pg_port_base="${PGWT_PG_PORT_BASE:-5400}"
    if [[ -z "$pg_major" ]]; then
        pg_major=$(readlink "/proc/$pm_pid/exe" 2>/dev/null | grep -oP 'postgresql/\K\d+(?=/)' || true)
    fi
    local pid_port=""
    local pm_cwd
    pm_cwd=$(readlink "/proc/$pm_pid/cwd" 2>/dev/null || true)
    if [[ -n "$pm_cwd" && -f "$pm_cwd/postmaster.pid" ]]; then
        pid_port=$(sed -n '4p' "$pm_cwd/postmaster.pid" 2>/dev/null || true)
        [[ "$pid_port" =~ ^[0-9]+$ ]] || pid_port=""
    fi
    if [[ -n "${PGWT_PGPORT:-}" ]]; then
        export PGPORT="$PGWT_PGPORT"
        echo "$label: PGPORT=$PGPORT (caller override via PGWT_PGPORT)"
    elif [[ -n "$pid_port" ]]; then
        export PGPORT="$pid_port"
        echo "$label: PGPORT=$PGPORT (read from postmaster.pid for PID $pm_pid)"
    elif [[ -n "$pg_major" ]]; then
        export PGPORT=$((pg_port_base + pg_major))
        echo "$label: PG major $pg_major -> PGPORT=$PGPORT (derived: PGWT_PG_PORT_BASE=$pg_port_base + PG$pg_major, postmaster.pid unreadable)"
    else
        echo "$label: ERROR: could not derive PGPORT for PID $pm_pid (postmaster.pid unreadable, no PG major, no PGWT_PGPORT set)" >&2
        exit 1
    fi
}

# check_pgbench_provisioned LABEL LOGFILE
# Fails loudly (never re-`pgbench -i`s) if the box's provisioned pgbench
# tables are missing -- `-i` drops and recreates every pgbench table, which
# would silently destroy the box's provisioned scale-10 dataset for every
# OTHER test sharing it (issue #93 review item 1).
check_pgbench_provisioned() {
    local label="$1" logfile="$2"
    echo "$label: checking for provisioned pgbench tables"
    local rows
    rows=$(psql -U postgres -d postgres -tAc \
        "SELECT count(*) FROM pgbench_accounts" 2>>"$logfile")
    if [[ -z "$rows" || "$rows" -lt 1 ]]; then
        echo "$label: ERROR: pgbench_accounts is missing/empty on PGPORT=$PGPORT --" \
             "tests/provision-runner.sh should have initialized it. Refusing to" \
             "silently 'pgbench -i' here (would destroy the box's provisioned" \
             "scale-10 dataset and change overhead_trend.csv's baseline for" \
             "every other test)."
        tail -n 20 "$logfile"
        exit 1
    fi
    echo "$label: pgbench_accounts has $rows rows (provisioned) -- reusing"
}
