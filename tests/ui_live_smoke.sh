#!/bin/bash
# ui_live_smoke.sh — issue #93 phase 2: runs tests/ui_live_smoke.py against a
# REAL daemon + REAL Go bridge (not tests/mock_server.py). This is the ONLY
# front-end for tests/ui_live_smoke.py that a human is expected to run
# directly; it is wired into tests/run_all.sh's root+PG live section, so
# `sudo tests/run_all.sh --require-live` (== `make box-check`) runs it too.
#
# What it does, in order (mirrors the proven start/stop patterns in
# tests/test_capture_smoke.py and tests/test_escalation.sh):
#   1. Controlled load: pgbench (4 clients, background, for the whole run) +
#      a LOOPING lock/sleep workload (tests/test_capture_smoke.py's Workload
#      class, fired repeatedly instead of once) so Lock:relation and
#      Timeout:PgSleep keep showing up in every live tick, not just the
#      first one.
#   2. pg_wait_tracer --daemon --mode full on a fresh temp trace dir.
#   3. The Go bridge (web/pgwt) against root@localhost, pointed at the
#      daemon's trace dir.
#   4. tests/ui_live_smoke.py --url against the bridge's local URL.
#   5. Reverse-order teardown, propagating ui_live_smoke.py's exit code
#      (verified: `exit N` inside a function whose EXIT trap's last command
#      fails/succeeds still leaves the process exit code at N — bash keeps
#      the code from the `exit` call that triggered the trap).
#
# DEVIATION from the issue's stated design ("Daemon in --mode tiered; trigger
# one escalation ... so the exact-tier panels (Transitions, Waterfall,
# Scatter, Matrix) have data"): measured on the gate box (2026-09-16),
# src/daemon.c gates the query__execute__start/done and query__plan__*
# USDT probes on `d->mode == PGWT_MODE_FULL` UNCONDITIONALLY -- an escalated
# window under --mode tiered never attaches them (the gate is checked once
# at daemon startup, not per-window). Executions/Waterfall/Scatter need
# those markers (src/server.c handle_executions pairs EXEC_START/EXEC_END);
# under tiered+escalate they showed "No executions for selected range" for
# the whole run, confirmed by reading src/daemon.c's own comment ("Plan/
# execute USDT probes remain full-mode-only, as before Stage 3"). Transitions
# and Matrix do NOT need these (they read the plain wait-event stream) and
# rendered correctly under tiered+escalate. --mode full has no escalation
# concept (always full fidelity; pgwt_escalate() would return "escalation
# requires --mode tiered"), so step 3's control-socket escalate is REMOVED,
# not merely skipped. Flagged as a Design question for the owner: whether to
# formally change the issue's design decision, or accept "no exact-tier data,
# ever" as the correct tiered-mode UI state for these two tabs and lower the
# bar for them specifically. Full mode is what makes today's evidence run
# actually exercise all 11 tabs with real data.
#
# RATIFIED (2026-09-17, lead): --mode full stays -- this test's job is to
# exercise every panel, and full is the only mode that feeds them all. The
# tiered+escalation USDT question above is being raised with the owner
# separately as its own decision, independent of this test.
#
# Requirements (gate box, tests/provision-runner.sh):
#   - root, a running PostgreSQL with pg_stat_statements preloaded.
#   - Playwright + Chromium installed for the box's python3 (same package
#     tests/run_all.sh's web-UI section already requires).
#   - root's OWN ssh public key present in root's authorized_keys, AND
#     localhost/127.0.0.1 already in root's known_hosts (web/bridge.go's
#     NewSSHBridge runs ssh with `-o BatchMode=yes`, which REFUSES rather
#     than prompts on an unknown host key or missing key auth -- a silent
#     hang here shows up as "bridge never answered /session" below with an
#     unhelpful log). Neither is set up by this script — provisioning's job.
#   - pg_wait_tracer, pgwt-server, web/pgwt already built (`make` /
#     `make pgwt-client`) — tests/run_all.sh's Step 0 does this.
#
# This script cannot be run on the Mac (no root, no PG, no BPF, no Linux) —
# it is exercised by `bash -n` only until it runs on the box in phase 2.
#
# LOCK DISCIPLINE (the gate box is shared): every CPU-using step this script
# runs -- make, Chromium/Playwright, ffmpeg video muxing, pgbench, the
# daemon -- MUST run inside `flock /tmp/pgwt-box-check.lock ...` on the box,
# exactly like scripts/box-check.sh's own `sudo tests/run_all.sh` call. This
# script does not grab the lock itself (run_all.sh's caller does, once, for
# the whole live section); a one-off manual invocation on the box (as in
# this file's own header example) MUST be wrapped the same way, e.g.:
#   flock /tmp/pgwt-box-check.lock sudo tests/ui_live_smoke.sh --pg-version 17
# A concurrent unlocked run caused transient test_cli misses for another
# agent sharing this box (2026-09-17) -- never run this, or any manual
# daemon/pgbench/bridge session, outside the lock.
#
# Usage: sudo tests/ui_live_smoke.sh [--pid POSTMASTER_PID] [--pg-version N]
#
# --pg-version N: on a multi-cluster box (the gate box runs PG 13/16/17/18
# at once, one postgres process each) this picks the postmaster via
# testutil.sh's find_postmaster --pg-version. PGPORT (Debian's pg_wrapper
# reads it, and it survives sudo) is derived either way, from whichever
# postmaster PID is actually resolved (--pid, --pg-version, or the bare
# find_postmaster fallback) -- so this also works correctly when run_all.sh's
# LIVE_TESTS invokes this script with only --pid, e.g. via `make box-check`.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
source "$SCRIPT_DIR/testutil.sh"
# stop_pid / url_ready / derive_pgport / check_pgbench_provisioned: shared
# with tests/demo_rehearsal.sh (issue #157) -- see live_daemon_lib.sh.
source "$SCRIPT_DIR/live_daemon_lib.sh"

TRACER="$PROJECT_DIR/pg_wait_tracer"
SERVER="$PROJECT_DIR/pgwt-server"
BRIDGE="$PROJECT_DIR/web/pgwt"

PM_PID=""
PG_MAJOR=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --pid) PM_PID="$2"; shift 2 ;;
        --pg-version) PG_MAJOR="$2"; shift 2 ;;
        *) echo "Usage: $0 [--pid POSTMASTER_PID] [--pg-version N]"; exit 1 ;;
    esac
done

if [[ -z "$PM_PID" ]]; then
    if [[ -n "$PG_MAJOR" ]]; then
        PM_PID=$(find_postmaster --pg-version "$PG_MAJOR")
    else
        PM_PID=$(find_postmaster)
    fi
fi
if [[ -z "$PM_PID" ]]; then
    echo "ERROR: cannot find postmaster PID"
    exit 1
fi
echo "ui_live_smoke: postmaster PID $PM_PID"

# PGPORT: three tiers, mirroring run_all.sh's own derivation (commit
# 863f086) exactly, for the same reason it exists there -- NEVER trust an
# ambient/inherited PGPORT (found the hard way running this exact script:
# the gate box sets PGPORT=5418 in /etc/environment, which every ssh session
# inherits; that silently overrode an explicit `--pg-version 13` and ran the
# whole walk against PG18's cluster while tracing PG13's postmaster, an
# 11/11 false failure -- precisely the "stale PGPORT silently traces one
# cluster while the load runs against another" bug). Factored into
# live_daemon_lib.sh's derive_pgport (issue #157) -- see its doc comment for
# the exact three tiers.
derive_pgport "$PM_PID" "$PG_MAJOR" "ui_live_smoke"

for bin in "$TRACER" "$SERVER" "$BRIDGE"; do
    if [[ ! -x "$bin" ]]; then
        echo "ERROR: $bin not built (make / make pgwt-client)"
        exit 1
    fi
done

# issue #174: web/bridge.go's NewSSHBridge runs `ssh -o BatchMode=yes root@
# localhost ...` with no StrictHostKeyChecking override, so it silently
# refuses (no prompt, BatchMode) the moment root's known_hosts entry for
# localhost/127.0.0.1 doesn't match the box's actual current host key. On a
# freshly booted VM (a cloud-init image regenerates its host keys per
# instance -- "ssh_deletekeys", the standard "don't reuse the imaged host's
# keys" behaviour) that mismatch shows up as every one of this script's
# eleven tabs failing at once with an opaque "WebSocket never reached
# #status.connected" many minutes into the run -- easy to mistake for a
# product regression. tests/hetzner-vm.sh/tests/provision-runner.sh already
# (re)scan the current key at VM-creation/provisioning time, so this should
# never fire in practice; this check exists purely to turn a real recurrence
# into ONE immediate, named failure instead of eleven confusing ones, and to
# fail in under a second rather than after the workload/daemon/bridge
# startup below.
#
# Review finding: an earlier version of this check attributed ANY ssh
# failure here to issue #174 -- a genuinely dead sshd, a missing
# authorized_keys, or a network hiccup would get the same confident
# "NOT a UI/bridge regression" misdiagnosis printed over the real error.
# Only claim the known-hosts cause when ssh's own output actually says so
# (its two documented strings for this exact failure); otherwise report the
# raw failure without attributing a cause -- a check that confidently names
# the wrong cause is worse than one that just fails.
echo "ui_live_smoke: checking root@localhost/127.0.0.1 ssh host-key trust (issue #174)"
ssh_hostkey_err=""
for h in localhost 127.0.0.1; do
    if ! ssh_out=$(ssh -o BatchMode=yes -o ConnectTimeout=5 "root@$h" true 2>&1); then
        ssh_hostkey_err="$ssh_hostkey_err
--- ssh -o BatchMode=yes root@$h true ---
$ssh_out"
    fi
done
if [[ -n "$ssh_hostkey_err" ]]; then
    if grep -qE 'Host key verification failed|REMOTE HOST IDENTIFICATION HAS CHANGED' <<<"$ssh_hostkey_err"; then
        echo "ERROR: ssh -o BatchMode=yes root@localhost failed -- this is issue #174" \
             "(a stale ~/.ssh/known_hosts entry for the loopback host key, typically" \
             "after a fresh boot regenerated it), NOT a UI/bridge regression." >&2
        echo "  Fix: ssh-keygen -R localhost -f ~/.ssh/known_hosts;" \
             "ssh-keygen -R 127.0.0.1 -f ~/.ssh/known_hosts;" \
             "ssh-keyscan -H localhost 127.0.0.1 >> ~/.ssh/known_hosts" >&2
        echo "  Or re-run: tests/provision-runner.sh ubuntu (its self-trust step redoes exactly this)." >&2
    else
        echo "ERROR: ssh -o BatchMode=yes root@localhost/127.0.0.1 failed before starting" \
             "the bridge -- cause unknown, does NOT match issue #174's known-hosts" \
             "signature (no 'Host key verification failed' / 'REMOTE HOST IDENTIFICATION" \
             "HAS CHANGED' in ssh's output below). Check sshd, authorized_keys, and" \
             "network reachability on this box." >&2
    fi
    echo "$ssh_hostkey_err" >&2
    exit 1
fi
echo "ui_live_smoke: ssh host-key trust OK"

# 30 min: measured on the gate box (2026-09-16/17), the real walk (real
# network + a real --mode full daemon, vs. the Mac --mock walk's ~3-4 min)
# took ~20-25 min end to end -- pgbench and the lock/sleep workload must
# outlast the WHOLE walk, not just its first minute, or the last few tabs
# (Waterfall, Scatter, Matrix) see a live window with no recent activity at
# all and legitimately render empty. Override for local iteration only --
# never lower this in a gating run.
DURATION_S="${PGWT_UI_LIVE_DURATION_S:-1800}"
PORT="${PGWT_UI_LIVE_PORT:-8384}"

TRACE_DIR=$(mktemp -d /tmp/pgwt_ui_live_XXXXXX)
DAEMON_LOG=$(mktemp /tmp/pgwt_ui_live_daemon_XXXXXX.log)
BRIDGE_LOG=$(mktemp /tmp/pgwt_ui_live_bridge_XXXXXX.log)
WORKLOAD_LOG=$(mktemp /tmp/pgwt_ui_live_workload_XXXXXX.log)
PGBENCH_LOG=$(mktemp /tmp/pgwt_ui_live_pgbench_XXXXXX.log)
SOCK="$TRACE_DIR/pgwt.sock"

TRACER_PID=""
BRIDGE_PID=""
WORKLOAD_PID=""
PGBENCH_PID=""
SMOKE_RC=1

# stop_pid: bounded wait (signal, poll, KILL) -- live_daemon_lib.sh. The
# bridge gets SIGINT specifically (see cleanup()).
cleanup() {
    # Reverse start order. The bridge gets SIGINT specifically: web/main.go
    # only handles os.Interrupt (SIGINT), not SIGTERM -- a plain TERM would
    # kill the bridge process without it ever calling bridge.Close(), which
    # is what tears down its ssh/pgwt-server child. An orphaned pgwt-server
    # inherits the flock fd and blocks every later box-check (review item 4).
    stop_pid "$WORKLOAD_PID" 10
    stop_pid "$PGBENCH_PID" 10
    stop_pid "$BRIDGE_PID" 10 INT
    stop_pid "$TRACER_PID" 15

    echo "ui_live_smoke: daemon log tail:"
    tail -n 40 "$DAEMON_LOG" 2>/dev/null | sed 's/^/  /'
    echo "ui_live_smoke: bridge log tail:"
    tail -n 40 "$BRIDGE_LOG" 2>/dev/null | sed 's/^/  /'
    echo "ui_live_smoke: workload log tail:"
    tail -n 20 "$WORKLOAD_LOG" 2>/dev/null | sed 's/^/  /'
    tail -n 20 "$PGBENCH_LOG" 2>/dev/null | sed 's/^/  /'

    # Verify nothing from THIS run survived teardown (review item 4): pgrep
    # on the trace dir's unique mktemp path -- it appears in the daemon's
    # own argv AND (via ssh's argv on localhost) the bridge's pgwt-server
    # child's -- plus the PIDs stop_pid was tracking directly. A leaked
    # child here blocks every later box-check by holding the flock fd open.
    sleep 1
    local leftover=""
    leftover="$(pgrep -f "$TRACE_DIR" 2>/dev/null || true)"
    local pid
    for pid in "$PGBENCH_PID" "$WORKLOAD_PID" "$BRIDGE_PID" "$TRACER_PID"; do
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            leftover="$leftover"$'\n'"$pid"
        fi
    done
    leftover="$(echo "$leftover" | sed '/^$/d')"

    rm -rf "$TRACE_DIR"
    rm -f "$DAEMON_LOG" "$BRIDGE_LOG" "$WORKLOAD_LOG" "$PGBENCH_LOG"

    if [[ -n "$leftover" ]]; then
        echo "ERROR: processes from this run survived teardown (would block every later box-check via the flock fd):"
        echo "$leftover" | while read -r p; do
            [[ -n "$p" ]] && ps -o pid,ppid,cmd -p "$p" 2>/dev/null
        done
        exit 1
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# url_ready: GET a URL, exit 0 iff it answers 200 -- live_daemon_lib.sh.

# ── 1. Controlled load ───────────────────────────────────────────────────────

# Reuse the box's PROVISIONED pgbench tables (tests/provision-runner.sh:
# scale 10) -- do NOT `pgbench -i` here (check_pgbench_provisioned,
# live_daemon_lib.sh; see its doc comment for why).
check_pgbench_provisioned "ui_live_smoke" "$PGBENCH_LOG"

echo "ui_live_smoke: starting pgbench (4 clients, ${DURATION_S}s, throttled)"
# --rate: measured on the gate box, an UNTHROTTLED 4-client pgbench against
# --mode full (every event captured, not sampled) for the full DURATION_S
# grows the executions dataset large enough that Waterfall's "latest
# execution" query stopped answering within the issue's 60s no-data budget
# by the time the walk reached it (~15-20 min in). 25 tx/s keeps Lock/
# Timeout/CPU/IO wait classes and a growing executions list genuinely
# present (the issue's actual requirement) without the volume a smoke test
# has no need to generate. Also lighter on shared box CPU -- see the lock-
# discipline note above the file.
pgbench -U postgres -d postgres -c 4 -T "$DURATION_S" --rate=25 \
    >>"$PGBENCH_LOG" 2>&1 &
PGBENCH_PID=$!
sleep 1
if ! kill -0 "$PGBENCH_PID" 2>/dev/null; then
    echo "ERROR: pgbench exited immediately after starting:"
    tail -n 20 "$PGBENCH_LOG"
    exit 1
fi

# Lock/Timeout: tests/live_loop_workload.py (issue #157: factored out so
# tests/demo_rehearsal.sh can run the identical loop for a longer window).
echo "ui_live_smoke: starting looping lock/sleep workload"
python3 "$SCRIPT_DIR/live_loop_workload.py" "$DURATION_S" >>"$WORKLOAD_LOG" 2>&1 &
WORKLOAD_PID=$!
sleep 2   # Workload.open_sessions() itself sleeps ~1.5s before its first check
if ! kill -0 "$WORKLOAD_PID" 2>/dev/null; then
    echo "ERROR: lock/sleep workload exited immediately after starting:"
    tail -n 40 "$WORKLOAD_LOG"
    exit 1
fi

# ── 2. Daemon (--mode full: see the DEVIATION note above) ───────────────────
# --daemon: long-running (reconnect on PG restart), no --count/--duration
# bound -- the UI walk's own length decides how long we need it, not a fixed
# capture budget (test_escalation.sh's short-lived --duration daemon is the
# wrong shape here). --mode full is always full-fidelity from the first
# tick -- no escalate call, no budget, no warm-up window needed.
echo "ui_live_smoke: starting daemon (--mode full, trace dir $TRACE_DIR)"
"$TRACER" --daemon --pid "$PM_PID" -i 1 -T "$TRACE_DIR" \
    --mode full -v \
    >/dev/null 2>"$DAEMON_LOG" &
TRACER_PID=$!

for _ in $(seq 1 60); do
    [[ -S "$SOCK" ]] && break
    if ! kill -0 "$TRACER_PID" 2>/dev/null; then
        echo "ERROR: daemon exited during startup:"
        tail -n 40 "$DAEMON_LOG"
        exit 1
    fi
    sleep 0.5
done
if [[ ! -S "$SOCK" ]]; then
    echo "ERROR: control socket never appeared at $SOCK"
    tail -n 40 "$DAEMON_LOG"
    exit 1
fi
echo "ui_live_smoke: control socket ready"

# ── 3. Go bridge ─────────────────────────────────────────────────────────────
echo "ui_live_smoke: starting Go bridge on :$PORT against root@localhost"
"$BRIDGE" --port "$PORT" --trace-dir "$TRACE_DIR" --server-path "$SERVER" \
    root@localhost >"$BRIDGE_LOG" 2>&1 &
BRIDGE_PID=$!

BASE_URL="http://localhost:$PORT/"
for _ in $(seq 1 60); do
    if url_ready "${BASE_URL}session"; then
        break
    fi
    if ! kill -0 "$BRIDGE_PID" 2>/dev/null; then
        echo "ERROR: bridge exited during startup:"
        tail -n 40 "$BRIDGE_LOG"
        exit 1
    fi
    sleep 0.5
done
if ! url_ready "${BASE_URL}session"; then
    echo "ERROR: bridge never answered ${BASE_URL}session"
    tail -n 40 "$BRIDGE_LOG"
    exit 1
fi
echo "ui_live_smoke: bridge ready at $BASE_URL"

# ── 4. The walk ───────────────────────────────────────────────────────────────
python3 "$SCRIPT_DIR/ui_live_smoke.py" --url "$BASE_URL" \
    --pgbench-pid "$PGBENCH_PID" --workload-pid "$WORKLOAD_PID"
SMOKE_RC=$?

exit "$SMOKE_RC"
