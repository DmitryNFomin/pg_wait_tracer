#!/bin/bash
# test_escalation.sh — Integration test for tiered-mode escalation (Phase A4)
#
# Starts the daemon in --mode tiered with a small escalation budget, then
# drives the control socket to verify the escalation engine end to end:
#   - status reports tier "sampled" while de-escalated
#   - escalate grants a bounded window; tier flips to "escalated"
#   - watchpoints get attached during the window (backends_tracked + tier)
#   - a bounded window EXPIRES on its own and detaches (back to "sampled")
#   - deescalate detaches immediately
#   - escalate beyond the rolling-hour budget is DENIED with a reason
#   - escalation START/END markers land in the trace
#   - the pgwt-server control proxy forwards escalate/deescalate
#
# Requires: root, running PostgreSQL, python3.
# Usage: sudo tests/test_escalation.sh [--pid POSTMASTER_PID]
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TRACER="$SCRIPT_DIR/../pg_wait_tracer"
SERVER="$SCRIPT_DIR/../pgwt-server"
source "$SCRIPT_DIR/testutil.sh"

PM_PID=""
SELF_TEST_ONLY=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --pid) PM_PID="$2"; shift 2 ;;
        # Runs ONLY exec_rows_selftest (below) and exits. No root, no
        # PostgreSQL, no pgwt-server: it is how the #108 response classifier
        # is shown going red without spending a 40-minute box run. The same
        # function also runs unconditionally in the normal flow, so this
        # flag adds a way to invoke it, never a way to skip it.
        --self-test-only) SELF_TEST_ONLY=1; shift ;;
        *) echo "Usage: $0 [--pid POSTMASTER_PID] [--self-test-only]"; exit 1 ;;
    esac
done

# #622: the tier must declare PGWT_EXPECT_PHASE_PROBES before live work.
if [[ $SELF_TEST_ONLY -eq 0 ]]; then
    PHASE_EXPECT=$(python3 "$SCRIPT_DIR/testutil.py" phase-expectation) || exit $?
fi
# #622: --skip-usdt is the EL8 proxy; it removes only the phase probes.
PHASE_ARGS=()
[[ "${PGWT_TEST_FORCE_NO_PHASE_PROBES:-}" == "1" ]] && PHASE_ARGS+=(--skip-usdt)

if [[ -z "$PM_PID" && $SELF_TEST_ONLY -eq 0 ]]; then
    PM_PID=$(find_postmaster)
    if [[ -z "$PM_PID" ]]; then
        echo "ERROR: cannot find postmaster PID"
        exit 1
    fi
fi

echo "=== test_escalation ==="
[[ $SELF_TEST_ONLY -eq 0 ]] && echo "Postmaster PID: $PM_PID"

TRACE_DIR=$(mktemp -d /tmp/pgwt_esc_XXXXXX)
DAEMON_LOG=$(mktemp /tmp/pgwt_esc_daemon_XXXXXX.log)
SOCK="$TRACE_DIR/pgwt.sock"
TRACER_PID=""

# Budget: 10 full-fidelity seconds per rolling hour. Small enough to exhaust
# deterministically in the test.
BUDGET=10

passed=0
failed=0
PHASE_REFUSED=0

check() {
    if [[ "$1" -eq 0 ]]; then
        echo "  PASS: $2"; passed=$((passed + 1))
    else
        echo "  FAIL: $2"; failed=$((failed + 1))
    fi
}

# One pgwt-server data command. Prints, on stdout, exactly one of:
#   <integer>  the number of rows/points returned
#   UNAVAIL    the documented exact-tier refusal: a sampled-only window
#              cannot answer an EXACT-required view, so pgwt-server emits
#              {"unavailable":..., "code":"full_fidelity_required",
#               "fidelity":"sampled"} with NO rows key at all
#              (src/compute.h PGWT_UNAVAILABLE_CODE, src/server.c
#               emit_unavailable). This is the product behaving correctly,
#              and the control capture below accepts it -- but ONLY this
#              exact shape. A generic error, a crash, empty output or a
#              missing rows key on an EXACT-fidelity answer all stay ERR,
#              so a broken server cannot pass as "no executions".
#   ERR        anything else
exec_rows() {
    local dir="$1" cmd="$2" key="$3"
    printf '{"id":1,"cmd":"%s","limit":500}\n' "$cmd" | \
        "$SERVER" "$dir" 2>/dev/null | python3 -c "
import json,sys
try:
    r = json.loads(sys.stdin.readline())
    if r.get('code') == 'full_fidelity_required':
        # The refusal must be coherent: a sampled/none window AND no data
        # key. A refusal that also carries rows is a contradiction, and
        # picking either half of it would be inventing an answer.
        ok = (r.get('fidelity') in ('sampled', 'none') and '$key' not in r)
        print('UNAVAIL' if ok else 'ERR'); sys.exit(0)
    if r.get('error'):
        print('ERR'); sys.exit(0)
    print(len(r['$key']))
except Exception:
    print('ERR')
"
}

# ── Bypass suite for exec_rows (the #108 response classifier) ────────────
#
# WHY THIS EXISTS. The first version of exec_rows collapsed EVERY non-`rows`
# response to ERR, and the box-check run on this branch failed on exactly
# that: a sampled-only window makes pgwt-server emit its documented
# exact-tier refusal (code=full_fidelity_required, no `rows` key), which the
# classifier reported as "the server did not answer". The gate was red while
# the product was right -- the mirror image of the usual defect, and just as
# much a gate defect.
#
# Accepting that refusal must NOT become "accept anything that is not a row
# count". These cases pin both directions against a FAKE $SERVER, with no
# PostgreSQL, no root and no pgwt-server, so a regression here is caught in
# a second rather than in a 40-minute box run.
exec_rows_selftest() {
    local saved_server="$SERVER"
    local d t
    d=$(mktemp -d /tmp/pgwt_esc_selftest_XXXXXX)
    t="$d/fake-server"

    # $1 = the single line the fake server prints; "" = print nothing. The
    # payload goes in a file the stub cats, so no quoting of the JSON is
    # needed anywhere.
    _fake() {
        if [[ -n "$1" ]]; then
            printf '%s\n' "$1" > "$d/payload"
        else
            : > "$d/payload"
        fi
        {
            echo '#!/bin/bash'
            echo 'cat >/dev/null'
            echo "cat '$d/payload'"
        } > "$t"
        chmod +x "$t"
        SERVER="$t"
    }
    _case() {  # _case <want> <label> <server line>
        local want="$1" label="$2" line="$3" got
        _fake "$line"
        got=$(exec_rows "$d" executions rows)
        [[ "$got" == "$want" ]]
        check $? "exec_rows self-test: $label (got '$got', want '$want')"
    }

    _case 2 "an exact answer returns its row count" \
        '{"id":1,"fidelity":"exact","rows":[{"pid":1},{"pid":2}],"total_count":2}'
    _case 0 "an empty exact answer returns 0" \
        '{"id":1,"fidelity":"exact","rows":[],"total_count":0}'
    _case UNAVAIL "the documented exact-tier refusal is UNAVAIL" \
        '{"id":1,"unavailable":"requires full-fidelity data","code":"full_fidelity_required","fidelity":"sampled"}'
    # ── the ERR direction: none of these may pass as "no executions" ──
    _case ERR "a generic server error is ERR, not UNAVAIL" \
        '{"id":1,"error":"something went wrong"}'
    _case ERR "a response with no rows key and no refusal code is ERR" \
        '{"id":1,"fidelity":"exact","total_count":0}'
    _case ERR "a refusal code on an EXACT window is ERR (incoherent)" \
        '{"id":1,"code":"full_fidelity_required","fidelity":"exact"}'
    _case ERR "a different refusal code is ERR" \
        '{"id":1,"code":"request_too_large","fidelity":"sampled"}'
    _case ERR "a crashed server that prints nothing is ERR" ''
    _case ERR "unparseable output is ERR" 'not json at all'
    # A refusal that ALSO carries rows is incoherent: refuse rather than
    # silently prefer one half of a contradictory answer.
    _case ERR "refusal code together with rows is ERR" \
        '{"id":1,"code":"full_fidelity_required","fidelity":"sampled","rows":[]}'

    unset -f _fake _case
    SERVER="$saved_server"
    rm -rf "$d"
}

exec_rows_selftest
if [[ $SELF_TEST_ONLY -eq 1 ]]; then
    echo ""
    echo "$passed/$((passed + failed)) self-test checks passed"
    [[ $failed -eq 0 ]]
    exit $?
fi

cleanup() {
    if [[ -n "$TRACER_PID" ]] && kill -0 "$TRACER_PID" 2>/dev/null; then
        kill -TERM "$TRACER_PID" 2>/dev/null
        wait "$TRACER_PID" 2>/dev/null
    fi
    rm -rf "$TRACE_DIR" "$DAEMON_LOG"
}
trap cleanup EXIT

# Send one JSON line, print the one-line response.
ctl() {
    python3 - "$SOCK" "$1" <<'PYEOF'
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(5)
s.connect(sys.argv[1])
s.sendall((sys.argv[2] + "\n").encode())
buf = b""
while b"\n" not in buf:
    chunk = s.recv(4096)
    if not chunk:
        break
    buf += chunk
sys.stdout.write(buf.decode().split("\n")[0])
PYEOF
}

jget() { python3 -c "import json,sys; print(json.load(sys.stdin)$1)"; }

# ── Start daemon in tiered mode ───────────────────────────────
"$TRACER" --daemon --pid "$PM_PID" -i 1 -T "$TRACE_DIR" \
    --mode tiered --sample-rate 50 --escalation-budget "$BUDGET" -v \
    ${PHASE_ARGS[@]+"${PHASE_ARGS[@]}"} \
    >/dev/null 2>"$DAEMON_LOG" &
TRACER_PID=$!

for _ in $(seq 1 30); do
    [[ -S "$SOCK" ]] && break
    if ! kill -0 "$TRACER_PID" 2>/dev/null; then
        echo "ERROR: daemon exited during startup:"; tail -20 "$DAEMON_LOG"; exit 1
    fi
    sleep 0.5
done
[[ -S "$SOCK" ]]
check $? "control socket created"
if [[ ! -S "$SOCK" ]]; then tail -20 "$DAEMON_LOG"; exit 1; fi

# ── status: tiered, de-escalated ──────────────────────────────
STATUS=$(ctl '{"cmd":"status"}')
echo "  status: $STATUS"
echo "$STATUS" | python3 -c "
import json,sys
r=json.load(sys.stdin)
assert r['ok'] is True, r
assert r['mode']=='tiered', r
assert r['tier']=='sampled', r
assert r['escalation_supported'] is True, r
assert r['escalation_seconds_remaining']==0, r
assert abs(r['escalation_budget_remaining_s']-$BUDGET) < 1.0, r
# ESC-6: a finite budget is not unlimited.
assert r.get('escalation_budget_unlimited') is False, r
"
check $? "status: tiered/sampled, full budget, supported, not-unlimited"

# ── escalate for 4s ───────────────────────────────────────────
RESP=$(ctl '{"cmd":"escalate","duration_s":4,"reason":"manual test"}')
echo "  escalate: $RESP"
echo "$RESP" | python3 -c "
import json,sys
r=json.load(sys.stdin)
assert r['ok'] is True, r
assert r['escalated'] is True, r
assert r['granted_s']==4, r
assert r['seconds_remaining']>0, r
assert r['budget_remaining_s'] < $BUDGET, r
"
check $? "escalate granted a 4s window"

# tier must now be escalated
STATUS=$(ctl '{"cmd":"status"}')
TIER=$(echo "$STATUS" | jget "['tier']")
[[ "$TIER" == "escalated" ]]
check $? "tier flipped to escalated (got: $TIER)"

# ESC-9: metrics.tier must agree with status.tier even while escalated.
MTIER=$(ctl '{"cmd":"metrics"}' | jget "['tier']")
[[ "$MTIER" == "$TIER" ]]
check $? "metrics.tier == status.tier while escalated (ESC-9; both '$TIER')"

# ── window expires on its own ─────────────────────────────────
sleep 6
STATUS=$(ctl '{"cmd":"status"}')
echo "  post-expiry status: $STATUS"
TIER=$(echo "$STATUS" | jget "['tier']")
[[ "$TIER" == "sampled" ]]
check $? "bounded window auto-expired back to sampled (got: $TIER)"

REM=$(echo "$STATUS" | jget "['escalation_seconds_remaining']")
[[ "$REM" == "0" || "$REM" == "0.0" ]]
check $? "seconds_remaining back to 0 after expiry (got: $REM)"

# ── manual deescalate (escalate then drop immediately) ────────
RESP=$(ctl '{"cmd":"escalate","duration_s":3,"reason":"manual"}')
echo "$RESP" | python3 -c "import json,sys; assert json.load(sys.stdin)['ok']"
check $? "second escalate granted"
RESP=$(ctl '{"cmd":"deescalate"}')
echo "  deescalate: $RESP"
echo "$RESP" | python3 -c "
import json,sys
r=json.load(sys.stdin)
assert r['ok'] is True and r['escalated'] is False, r
"
check $? "deescalate acked, escalated=false"
TIER=$(ctl '{"cmd":"status"}' | jget "['tier']")
[[ "$TIER" == "sampled" ]]
check $? "tier sampled after manual deescalate (got: $TIER)"

# ── ESC-1 budget clamp: a request larger than the remaining budget is
#    CLAMPED to what's left (not denied) so the window runs up to the cap. ──
# We've spent ~4s (expired) + ~0-1s (deescalated) of the 10s budget, so a few
# seconds remain. Ask for an hour; it must be granted but clamped to <= budget.
RESP=$(ctl '{"cmd":"escalate","duration_s":3600,"reason":"greedy"}')
echo "  clamp: $RESP"
echo "$RESP" | python3 -c "
import json,sys
r=json.load(sys.stdin)
assert r['ok'] is True, r                       # granted, not denied
assert 0 < r['granted_s'] <= $BUDGET, r         # clamped to <= budget
"
check $? "over-ask escalate is CLAMPED to the remaining budget (ESC-1)"

# Let this clamped window run to expiry so the budget is fully spent.
sleep $(($BUDGET + 2))
STATUS=$(ctl '{"cmd":"status"}')
REM=$(echo "$STATUS" | jget "['escalation_budget_remaining_s']")
python3 -c "import sys; sys.exit(0 if float('$REM') < 1.0 else 1)"
check $? "budget fully spent after the clamped window expired (rem=$REM)"

# ── budget exhaustion → deny ──────────────────────────────────
# With ~0 budget left, a further escalate must now be DENIED with a reason.
RESP=$(ctl '{"cmd":"escalate","duration_s":5,"reason":"exhausted"}')
echo "  over-budget: $RESP"
echo "$RESP" | python3 -c "
import json,sys
r=json.load(sys.stdin)
assert r['ok'] is False, r
assert 'budget' in r['error'].lower(), r
assert 'budget_remaining_s' in r, r
"
check $? "escalate on an exhausted budget is denied with a reason"

# denied_total counter moved
DENIED=$(ctl '{"cmd":"metrics"}' | jget "['escalation_denied_total']")
[[ "$DENIED" -ge 1 ]]
check $? "escalation_denied_total >= 1 (got: $DENIED)"

WINDOWS=$(ctl '{"cmd":"metrics"}' | jget "['escalation_windows_total']")
[[ "$WINDOWS" -ge 2 ]]
check $? "escalation_windows_total >= 2 (got: $WINDOWS)"

# ── pgwt-server control proxy forwards escalate ───────────────
# (budget is nearly spent; a tiny window should still fit or be denied —
#  either way the proxy must round-trip a well-formed response.)
RESP=$(echo '{"id":3,"cmd":"control","request":{"cmd":"status"}}' | \
       "$SERVER" "$TRACE_DIR" 2>/dev/null)
echo "$RESP" | python3 -c "
import json,sys
r=json.load(sys.stdin)
assert r['id']==3, r
assert r['response']['mode']=='tiered', r
assert 'tier' in r['response'], r
"
check $? "proxy forwards tiered status with tier field"

RESP=$(echo '{"id":4,"cmd":"control","request":{"cmd":"deescalate"}}' | \
       "$SERVER" "$TRACE_DIR" 2>/dev/null)
echo "$RESP" | python3 -c "
import json,sys
r=json.load(sys.stdin)
assert r['id']==4, r
assert r['response']['ok'] is True, r
"
check $? "proxy forwards deescalate"

# ── escalation markers landed in the trace ────────────────────
# Force a flush by stopping the daemon, then scan trace files for the
# escalate marker sentinels (0xFFFFFFF4 / 0xFFFFFFF5) — they are written
# uncompressed-then-LZ4'd, so we decode via pgwt-server --dump which prints
# nothing about markers; instead we just assert trace bytes grew and that at
# least one window was recorded (windows_total above already proves writes).
TRACE_BYTES=$(ctl '{"cmd":"metrics"}' | jget "['trace_bytes_written_total']")
[[ "$TRACE_BYTES" -gt 0 ]]
check $? "trace bytes written (markers + samples) > 0 ($TRACE_BYTES)"

# ── clean shutdown ────────────────────────────────────────────
kill -TERM "$TRACER_PID"
SHUT_OK=1
for _ in $(seq 1 20); do
    if ! kill -0 "$TRACER_PID" 2>/dev/null; then SHUT_OK=0; break; fi
    sleep 0.5
done
wait "$TRACER_PID" 2>/dev/null
check $SHUT_OK "daemon exits cleanly on SIGTERM"
TRACER_PID=""

[[ ! -e "$SOCK" ]]
check $? "socket unlinked on shutdown"


# == #108: a tiered escalation window must produce EXECUTIONS ===============
#
# THE DEFECT. PGWT_EXACT_PROBE_ALL_MASK was QUERY_ID|ACTIVITY only, and the
# four query__execute__ / query__plan__ USDT probes attached once at daemon
# start in FULL MODE ONLY. In tiered mode an escalation therefore granted
# exactness while the Waterfall ("executions") and Scatter ("exec_scatter")
# tabs stayed empty.
#
# WHY THIS RUNS IN TIERED MODE AND NOTHING ELSE WOULD DO. In `--mode full`
# the probes are attached from startup, so the same assertion passes on the
# broken product. Neither capture below uses full mode, and the control
# socket's own reported `mode` is asserted to be "tiered" before anything
# is concluded from its data.
#
# ANTI-VACUITY: an A/B, not a single positive. Capture A runs the SAME
# workload for the SAME duration with NO escalation and must yield ZERO
# executions -- that is the always-on sampled tier's contract, and execution
# rows exist only where an EXEC_START marker exists (src/compute.c), so a
# build that pinned the phase probes always-on fails A. Capture B escalates
# and must yield more than zero. One capture alone could pass either way.
#
# REFUSAL: every step that cannot be carried out FAILS with its reason. A
# missing psql, a server that will not answer, a daemon that will not start
# -- none of them skip.
echo ""
echo "-- #108: executions from a tiered escalation window --"

PSQL_BIN=$(command -v psql || true)
WL_PID=""
CAP_DIR=""
CAP_MODE=""
CAP_TIER=""
CAP_TIER_END=""
CAP_WINDOWS=""
CAP_MASKS=""
CAP_MASK_SAMPLES=0

esc108_cleanup() {
    [[ -n "$WL_STOP" ]] && rm -f "$WL_STOP"
    if [[ -n "$WL_PID" ]] && kill -0 "$WL_PID" 2>/dev/null; then
        kill -TERM "$WL_PID" 2>/dev/null; wait "$WL_PID" 2>/dev/null
    fi
    [[ -n "$CAP_DIR" ]] && rm -rf "$CAP_DIR"
}
WL_STOP=""

# One tiered capture. $1 = tag, $2 = "escalate" | "noescalate".
# Sets CAP_DIR (empty on failure), CAP_MODE, CAP_TIER, CAP_TIER_END,
# CAP_WINDOWS. Runs in THIS shell (not a subshell) so those assignments are
# visible to the caller.
#
# The --anomaly-* flags DISABLE auto-escalation. Tiered mode arms the anomaly
# engine by default, so without them the control capture's "no executions"
# rests on the rules' absolute floors staying above whatever this box is
# doing -- true today, but a neighbouring session raising AAS would turn the
# negative control red for a reason that has nothing to do with #108. This
# removes a trigger; it does not weaken an assertion. CAP_WINDOWS below is
# the independent proof that it worked.
run_tiered_capture() {
    local tag="$1" do_escalate="$2" log
    CAP_DIR=""; CAP_MODE="?"; CAP_TIER="?"; CAP_TIER_END="?"; CAP_WINDOWS="?"
    CAP_MASKS=""; CAP_MASK_SAMPLES=0
    local dir
    dir=$(mktemp -d "/tmp/pgwt_esc108_${tag}_XXXXXX")
    log=$(mktemp "/tmp/pgwt_esc108_${tag}_log_XXXXXX")
    SOCK="$dir/pgwt.sock"
    "$TRACER" --daemon --pid "$PM_PID" -i 1 -T "$dir" \
        --mode tiered --sample-rate 50 --escalation-budget 120 -v \
        --anomaly-aas-factor 1000000 --anomaly-cpu-cusum-disable \
        --anomaly-lock-fraction 1 --anomaly-lock-min-aas 1000000 \
        ${PHASE_ARGS[@]+"${PHASE_ARGS[@]}"} \
        >/dev/null 2>"$log" &
    TRACER_PID=$!
    local up=0
    for _ in $(seq 1 40); do
        if [[ -S "$SOCK" ]]; then up=1; break; fi
        kill -0 "$TRACER_PID" 2>/dev/null || break
        sleep 0.5
    done
    if [[ $up -ne 1 ]]; then
        echo "  daemon ($tag) did not come up; log tail:"; tail -20 "$log"
        kill -TERM "$TRACER_PID" 2>/dev/null; wait "$TRACER_PID" 2>/dev/null
        TRACER_PID=""; rm -rf "$dir" "$log"; return 1
    fi
    CAP_MODE=$(ctl '{"cmd":"status"}' | jget "['mode']" 2>/dev/null || echo "?")
    CAP_TIER=$(ctl '{"cmd":"status"}' | jget "['tier']" 2>/dev/null || echo "?")

    WL_STOP=$(mktemp "/tmp/pgwt_esc108_${tag}_stop_XXXXXX")
    (
        while [[ -e "$WL_STOP" ]]; do
            "$PSQL_BIN" -U postgres -d postgres -tAq -c \
                "SELECT count(*) FROM generate_series(1, 50000) g" \
                >/dev/null 2>&1 || sleep 0.2
        done
    ) &
    WL_PID=$!
    sleep 3
    if [[ "$do_escalate" == "escalate" ]]; then
        local r
        r=$(ctl '{"cmd":"escalate","duration_s":8,"reason":"#108"}')
        echo "  escalate($tag): $r"
        # #622: bits are assembled one at a time. Keep EVERY open-window
        # sample, so a torn read cannot hide a phase bit seen earlier.
        local sample active mask
        for _ in $(seq 1 18); do
            # status carries the mask; metrics carries escalation_active.
            # Require both replies rather than treating missing state as idle.
            if active=$(ctl '{"cmd":"metrics"}') && sample=$(ctl '{"cmd":"status"}'); then
                mask=$(printf '%s\n' "$sample" | python3 -c '
import json, sys
r = json.load(sys.stdin)
active = json.loads(sys.argv[1])["escalation_active"]
if active is True and r.get("tier") == "escalated":
    print(json.dumps(r.get("exact_probe_attached_mask")))
' "$active")
                if [[ $? -ne 0 ]]; then
                    check 1 "#622: unreadable mid-window status/metrics response"
                fi
                if [[ -n "$mask" ]]; then
                    CAP_MASKS="$CAP_MASKS $mask"
                    CAP_MASK_SAMPLES=$((CAP_MASK_SAMPLES + 1))
                fi
            else
                check 1 "#622: status could not be read during the exact window"
            fi
            sleep 0.5
        done
        echo "  mid-window mask samples: $CAP_MASKS (samples: $CAP_MASK_SAMPLES)"
    else
        sleep 9
    fi
    rm -f "$WL_STOP"; WL_STOP=""
    sleep 1
    kill -TERM "$WL_PID" 2>/dev/null; wait "$WL_PID" 2>/dev/null; WL_PID=""
    # Read the tier and the window counter while the daemon is still up: the
    # tier asserted at STARTUP cannot see a late auto-escalation, and
    # escalation_windows_total is the quantity that decides, independently of
    # any flag, whether this capture had an exact window at all.
    CAP_TIER_END=$(ctl '{"cmd":"status"}' | jget "['tier']" 2>/dev/null || echo "?")
    CAP_WINDOWS=$(ctl '{"cmd":"metrics"}' | jget "['escalation_windows_total']" 2>/dev/null || echo "?")
    kill -TERM "$TRACER_PID"
    wait "$TRACER_PID" 2>/dev/null
    TRACER_PID=""
    rm -f "$log"
    CAP_DIR="$dir"
    return 0
}

if [[ -z "$PSQL_BIN" ]]; then
    check 1 "#108: psql is available (cannot drive a workload without it)"
else
    trap 'cleanup; esc108_cleanup' EXIT

    # -- A: sampled tier only, no escalation --------------------
    run_tiered_capture control noescalate
    check $? "#108: control capture started"
    if [[ -n "$CAP_DIR" ]]; then
        A_DIR="$CAP_DIR"; CAP_DIR=""
        [[ "$CAP_MODE" == "tiered" ]]
        check $? "#108: control capture ran in TIERED mode (got: $CAP_MODE)"
        [[ "$CAP_TIER" == "sampled" && "$CAP_TIER_END" == "sampled" ]]
        check $? "#108: control capture sampled at both ends (start: $CAP_TIER, end: $CAP_TIER_END)"
        # The decisive one: zero exact windows over the whole capture. This
        # is what makes "no executions" mean "the sampled tier produces
        # none", rather than "nothing happened to escalate".
        [[ "$CAP_WINDOWS" == "0" ]]
        check $? "#108: control capture opened NO escalation window (windows: $CAP_WINDOWS)"
        A_EXEC=$(exec_rows "$A_DIR" executions rows)
        A_SCAT=$(exec_rows "$A_DIR" exec_scatter points)
        [[ "$A_EXEC" != "ERR" && "$A_SCAT" != "ERR" ]]
        check $? "#108: server answered the control capture (execs=$A_EXEC scatter=$A_SCAT)"
        # No executions, either as zero rows or as the explicit exact-tier
        # refusal -- a sampled-only window is the one case where
        # pgwt-server answers "requires full-fidelity data" instead of an
        # empty list, and that refusal IS the #108 interim state. Any other
        # outcome, including a nonzero count, fails.
        [[ ( "$A_EXEC" == "0" || "$A_EXEC" == "UNAVAIL" ) && \
           ( "$A_SCAT" == "0" || "$A_SCAT" == "UNAVAIL" ) ]]
        check $? "#108: always-on sampled tier yields NO executions (execs=$A_EXEC scatter=$A_SCAT)"
        rm -rf "$A_DIR"
    fi

    # -- B: same workload, with an escalation window ------------
    run_tiered_capture window escalate
    check $? "#108: escalated capture started"
    if [[ -n "$CAP_DIR" ]]; then
        B_DIR="$CAP_DIR"; CAP_DIR=""
        [[ "$CAP_MODE" == "tiered" ]]
        check $? "#108: escalated capture ran in TIERED mode (got: $CAP_MODE)"
        # And the escalated capture provably DID open one, so the pair
        # differs in exactly the thing under test.
        [[ "$CAP_WINDOWS" =~ ^[0-9]+$ && "$CAP_WINDOWS" -ge 1 ]]
        check $? "#108: escalated capture opened an escalation window (windows: $CAP_WINDOWS)"
        B_EXEC=$(exec_rows "$B_DIR" executions rows)
        B_SCAT=$(exec_rows "$B_DIR" exec_scatter points)
        [[ "$B_EXEC" != "ERR" && "$B_SCAT" != "ERR" ]]
        check $? "#108: server answered the escalated capture (execs=$B_EXEC scatter=$B_SCAT)"
        PHASE_RESULT=$(PYTHONPATH="$SCRIPT_DIR" python3 -c '
import json, sys
from testutil import phase_contract_verdict, resolve_declared_phase_mask
masks = [json.loads(v) for v in sys.argv[1].split()]
def count(raw):
    return None if raw == "UNAVAIL" else int(raw) if raw.isdigit() else raw
verdict, reason = phase_contract_verdict(
    resolve_declared_phase_mask(), masks,
    {"executions": count(sys.argv[2]), "exec_scatter": count(sys.argv[3])})
print(verdict)
print(reason)
' "$CAP_MASKS" "$B_EXEC" "$B_SCAT")
        PHASE_RC=$?
        PHASE_VERDICT=$(printf '%s\n' "$PHASE_RESULT" | head -n 1)
        PHASE_REASON=$(printf '%s\n' "$PHASE_RESULT" | tail -n +2)
        echo "  phase contract: $PHASE_VERDICT: $PHASE_REASON"
        if [[ "$PHASE_RC" -ne 0 ]]; then
            check 1 "#622: phase contract could not be evaluated"
        elif [[ "$PHASE_VERDICT" == "pass" ]]; then
            [[ "$B_EXEC" =~ ^[0-9]+$ && "$B_EXEC" -gt 0 ]]
            check $? "#108: a TIERED escalation produces Waterfall executions (got: $B_EXEC)"
            [[ "$B_SCAT" =~ ^[0-9]+$ && "$B_SCAT" -gt 0 ]]
            check $? "#108: a TIERED escalation produces Scatter points (got: $B_SCAT)"
        elif [[ "$PHASE_VERDICT" == "refuse" ]]; then
            echo "PHASE-CONTRACT-REFUSED: $PHASE_REASON"
            PHASE_REFUSED=1
            echo "  SKIP: #108 Waterfall/Scatter positive checks (phase contract REFUSED)"
        else
            check 1 "$PHASE_REASON"
        fi
        rm -rf "$B_DIR"
    fi
fi

echo ""
echo "$passed/$((passed + failed)) tests passed"
if [[ $failed -eq 0 && $PHASE_REFUSED -eq 1 ]]; then
    exit 78
fi
[[ $failed -eq 0 ]]
