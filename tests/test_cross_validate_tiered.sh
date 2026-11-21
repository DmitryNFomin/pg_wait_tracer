#!/bin/bash
# test_cross_validate_tiered.sh — A4 cross-validation (sampled vs exact).
#
# THE key A4 test. Runs the daemon in --mode tiered under a pgbench workload,
# escalates to full fidelity for a window, then compares the sampled
# estimators against the exact transition data over the SAME window using the
# cross_validate tool. Used to choose/justify the default --sample-rate.
#
# Sweeps a list of sample rates. Issue #309: this used to exit 0 as soon as
# ANY ONE rate passed, so a run where 3 of 4 rates were outside tolerance
# still reported overall success -- "24/24 green" across several boxes was
# not evidence any given rate, including the shipped default, actually held
# up. Every rate in RATES must now be within tolerance for exit 0; one
# out-of-tolerance rate fails the run (79 only for the bounded #675 signature;
# 80 for an invalid workload, otherwise 1, including an unmeasured rate). The first rate
# that PASSES is still printed as the recommended default -- that framing is
# unchanged -- but it
# no longer decides the exit code.
#
# Requires: root, running PostgreSQL, pgbench, cross_validate built.
# Usage: sudo tests/test_cross_validate_tiered.sh [--pid PID] [--rates "10 50 100"]
#                                                 [--esc-seconds 60] [--clients 8]
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TRACER="$SCRIPT_DIR/../pg_wait_tracer"
XVAL="$SCRIPT_DIR/cross_validate"
CPU_GROUP_READER="$SCRIPT_DIR/cpu_group_readback"
source "$SCRIPT_DIR/testutil.sh"
source "$SCRIPT_DIR/cross_validate_rate_output.sh"
source "$SCRIPT_DIR/xval_tolerance.sh"

PM_PID=""
RATES="10 50 100 200"
ESC_SECONDS=60
CLIENTS=8
# +/-8.8 percentage points, DERIVED. Was 10 pp from the A4 tool's original
# commit (eaf31b0) with no derivation anywhere in the repo, and this comment
# used to say so -- "treat 10pp as a value someone should revisit, not a
# measured bound". Issue #309 asked for a derivation; this is it.
#
#   bound = BIAS + z(K) * 100*sqrt(0.25/N)
#         = 6.52 + 2.5688 * 0.8937  =  8.8 pp
#
#   BIAS = 6.52 pp   systematic CPU under-read at the SHIPPED operating point
#                    (#306 arm A median, SCHED_OTHER, 10 Hz, 8 clients). Not
#                    3.9 pp -- that is arm B under SCHED_FIFO, which the
#                    product does not ship.
#   N    = 3130      smallest observed "Samples in window" at that operating
#                    point; samples are (tick, backend) pairs, so a 60 s
#                    10 Hz run yields thousands, not 600.
#   z(5) = 2.5688    95% FAMILY-WISE quantile over the K=5 events that clear
#                    cross_validate's 2% eligibility floor. The gated metric
#                    is a MAXIMUM over those events, so a per-event 2-sigma
#                    figure would cover ~23% of runs, not 95%. #321 was
#                    opened as a "stable 6 pp regression" and withdrawn by
#                    its own author for exactly this reason.
#
# The full derivation, every input's provenance, what the bound does NOT
# cover, and the guard that keeps this constant equal to its derivation live
# in tests/xval_tolerance.sh -- which `make check` runs on every push via
# tests/test_xval_tolerance_derivation.sh. Editing the number here without
# editing the derivation there (or vice versa) turns `make check` red; that
# is deliberate, and is the only reason this comment cannot rot.
#
# Derived for 10 Hz and applied to every rate in the sweep: the published
# post-#310 gate-1 medians fall with rate (6.0 / 6.4 / 5.5 / 2.5 pp at
# 10 / 50 / 100 / 200 Hz), so the 10 Hz bound is the loosest of the four. A
# 200 Hz pass is therefore NOT evidence that rate is inside a bound derived
# for it; that needs its own n>=5.
#
# NOT a certificate of accuracy: 6.52 of the 8.8 pp is #306's OPEN residual
# under-read, which this bound discloses rather than explains.
TOLERANCE=8.8
TOLERANCE_OVERRIDDEN=0

# Pure decision used below and by the offline runner self-test. Each arm is a
# comma-separated list of signed (exact - sampled) eligible-event deviations.
# Exit 79 is reserved here for #675's bounded tolerance-only signature; it is
# distinct from run_all's 78 phase refusal and shell 126/127 and 128+ signals.
# Exit 80 means an INVALID workload (rules b' / c below). run_all.sh's
# expected-exit mismatch branch counts 80 as a real failure on the #675 tier;
# its ordinary nonzero branch does so everywhere else. It is neither 78 nor
# the separate exit-93 recovery signature.
# #675 / #760 record 9.8-12.4 pp on EL8 4.18; the two #760 review runs measured
# 9.8-12.4 and 10.1-11.1 pp. 15.0 pp is a regression tripwire for an OPEN
# defect, NOT an accuracy claim or a wider tolerance. Delete it and exit 79
# with the KNOWN_FAILING entry when #675 is fixed.
tiered_classify() {
    python3 - "$@" <<'PYEOF'
import math
import sys

try:
    other_failure, tolerance, *arms = sys.argv[1:]
    tolerance = float(tolerance)
    if "WORKLOAD_INVALID" in arms:
        sys.exit(80)
    if other_failure != "0" or not arms or not math.isfinite(tolerance) or tolerance <= 0:
        sys.exit(1)
    outside = False
    peak = 0.0
    for arm in arms:
        for token in arm.split(","):
            delta = float(token)
            if not math.isfinite(delta):
                sys.exit(1)
            peak = max(peak, abs(delta))
            if abs(delta) > tolerance:
                if delta < 0:  # sampled over-reading exact is a different defect
                    sys.exit(1)
                outside = True
    if outside and peak > 15.0:
        sys.exit(1)
    sys.exit(79 if outside else 0)
except ValueError:
    sys.exit(1)
PYEOF
}

# Read ALL eligible event deltas, not just the maximum's unsigned headline.
# cross_validate also gates top-N overlap: a failure there must never be #675.
# Missing/malformed output, a tool error, or inconsistent verdict is INVALID.
tiered_arm_assertions() {
    python3 - "$1" "$2" "$3" <<'PYEOF'
import re
import sys

def require(condition):
    if not condition:
        raise ValueError("invalid comparator result")

output, rc, tolerance = sys.argv[1:]
try:
    table = re.search(r"^wait_event\s+exact_share%\s+sampled%\s+delta_pp\n-+\n(.*?)\n\n",
                      output, re.M | re.S)
    require(table)
    deltas = []
    number = r"([0-9]+(?:\.[0-9]+)?(?:e[+-]?[0-9]+)?)"
    for line in table[1].splitlines():
        row = re.fullmatch(r"\S+\s+" + number + r"%\s+" + number +
                           r"%\s+([0-9]+\.[0-9]{1,2})", line)
        require(row)
        exact, sampled, printed_delta = map(float, row.groups())
        delta = abs(exact - sampled)
        require(0 <= exact <= 100 and 0 <= sampled <= 100)
        require(abs(delta - printed_delta) <= 0.011)  # delta printed to 0.01 pp
        if exact >= 2.0:  # cross_validate's default --min-share
            deltas.append(delta if exact >= sampled else -delta)
    require(deltas)
    overlaps = re.findall(r"^Top-([0-9]+) event overlap: ([0-9]+)/([0-9]+)$", output, re.M)
    require(len(overlaps) == 1)
    top, overlap, denominator = map(int, overlaps[0])
    require(1 <= top <= 5 and denominator == top and min(top, 4) <= overlap <= top)
    maxima = re.findall(r"^Max share disagreement \(events >= 2% exact share\): ([0-9]+\.[0-9]{1,2}) pp \(.+\)$", output, re.M)
    require(len(maxima) == 1 and abs(float(maxima[0]) - max(map(abs, deltas))) <= 0.011)
    verdicts = re.findall(r"^RESULT: (PASS|FAIL)$", output, re.M)
    require(len(verdicts) == 1)
    # Shares are printed at round-trip precision: recomputing their difference
    # reproduces the comparator, even when a true 8.801 prints as 8.80 pp.
    # Two decimals alone would merely shrink the old rounding gap. Legacy
    # one-decimal fixtures remain readable; current decisions use the shares.
    passed = max(map(abs, deltas)) <= float(tolerance)
    require(verdicts[0] == ("PASS" if passed else "FAIL"))
    require(rc == ("0" if passed else "1"))
    print(",".join(str(delta) for delta in deltas))
except ValueError:
    print("INVALID")
PYEOF
}

# Pure per-run validity check. Progress timestamps are Unix epoch seconds
# (--progress-timestamp); only complete 5 s intervals inside escalation count.
# The median belongs to this run's escalated intervals, not to another run.
tiered_workload_validity() {
    python3 - "$@" <<'PYEOF'
import math
from pathlib import Path
import re
import statistics
import sys

output, progress_path, start, end = sys.argv[1:]
try:
    start, end = float(start), float(end)
    if not (math.isfinite(start) and math.isfinite(end) and start < end):
        raise ValueError("unreadable escalation window")
    intervals = []
    for line in Path(progress_path).read_text().splitlines():
        if not line.startswith("progress:"):
            continue
        match = re.match(r"progress: ([0-9]+(?:\.[0-9]+)?)(?: s)?, "
                         r"([0-9]+(?:\.[0-9]+)?) tps,", line)
        if not match:
            raise ValueError("unreadable progress interval: " + line)
        timestamp, tps = map(float, match.groups())
        if not (math.isfinite(timestamp) and math.isfinite(tps)):
            raise ValueError("non-finite progress interval: " + line)
        if start <= timestamp - 5 and timestamp <= end:
            intervals.append((timestamp, tps))
    if not intervals:
        raise ValueError("no complete 5 s progress intervals inside escalation")
    median = statistics.median(tps for _, tps in intervals)
    stalled = [(timestamp, tps) for timestamp, tps in intervals if tps < 0.5 * median]
    if stalled:
        print("b': median=%.1f tps; below 50%%=%s; intervals=%s" %
              (median, stalled, intervals))
        sys.exit(0)
    cpu = re.findall(r"^CPU\s+([0-9]+(?:\.[0-9]+)?(?:e[+-]?[0-9]+)?)%\s", output, re.M)
    if len(cpu) != 1 or not 0 <= float(cpu[0]) <= 100:
        print("c: exact CPU share unreadable; CPU rows=" + repr(cpu))
    elif float(cpu[0]) < 30:
        print("c: exact CPU share=%s%% < 30%%" % cpu[0])
    else:
        print("VALID")
except (OSError, ValueError) as exc:
    print("b': " + str(exc))
PYEOF
}

# These are separate from workload validity: a wrong deployment layout must
# exit 80 before any comparator can print PASS or a tolerance-only exit 79.
tiered_layout_readback() {
    local daemon="$1" backend="$2" pgbench="$3" daemon_group backend_group pgbench_group nice
    daemon_group=$("$CPU_GROUP_READER" "$daemon") || daemon_group="unknown: $daemon_group"
    backend_group=$("$CPU_GROUP_READER" "$backend") || backend_group="unknown: $backend_group"
    pgbench_group=$("$CPU_GROUP_READER" "$pgbench") || pgbench_group="unknown: $pgbench_group"
    nice=$(python3 - "$daemon" <<'PYEOF'
from pathlib import Path
import sys
try:
    # Linux nice is field 19; comm (field 2) can itself contain ')'.
    pid = int(sys.argv[1])
    print(int(Path(f"/proc/{pid}/task/{pid}/stat").read_text().rsplit(")", 1)[1].split()[16]))
except (OSError, ValueError, IndexError) as exc:
    print("unknown: " + str(exc))
PYEOF
) || nice="unknown: stat reader failed"
    echo "  LAYOUT daemon pid=$daemon group=$daemon_group"
    echo "  LAYOUT backend pid=$backend group=$backend_group"
    echo "  LAYOUT pgbench pid=$pgbench group=$pgbench_group"
    echo "  LAYOUT sampler tid=$daemon nice=$nice"
    # Raw evidence beside the shared resolver's verdict: these records do
    # not decide placement, and allow the readbacks to be checked by hand.
    python3 - "$daemon" "$backend" "$pgbench" <<'PYEOF'
from pathlib import Path
import sys
for role, pid in zip(("daemon", "backend", "pgbench"), sys.argv[1:]):
    try:
        text = Path(f"/proc/{int(pid)}/cgroup").read_text()
        print(f"  RAW_CGROUP {role} pid={pid}: {text!r}")
        for line in text.splitlines():
            if line.startswith("0::"):
                path = Path("/sys/fs/cgroup") / line[3:].lstrip("/")
                while path != Path("/sys/fs/cgroup"):
                    path = path.parent
                    control = path / "cgroup.subtree_control"
                    print(f"  RAW_CPU_CONTROL {role} {control}: {control.read_text()!r}")
    except (OSError, ValueError) as exc:
        print(f"  RAW_CGROUP {role} unavailable: {exc}")
PYEOF
    if [[ "$daemon_group" == unknown* || "$backend_group" == unknown* ||
          "$pgbench_group" == unknown* || "$daemon_group" != "$backend_group" ||
          "$pgbench_group" == "$daemon_group" || "$nice" != -10 ]]; then
        echo "INVALID layout: daemon=$daemon_group backend=$backend_group pgbench=$pgbench_group sampler_nice=$nice (need daemon=backend, pgbench outside, nice=-10; exit 80)"
        return 80
    fi
}

# Own every process through its systemd unit, including startup failures and
# signals. Register before launching anything. Unique names avoid collisions;
# --collect unloads failed units too. Stopping the scope kills all pgbench jobs.
TRACER_UNIT=""; LOAD_SCOPE=""; LOAD_LAUNCH=""
TRACE_DIR=""; LOG=""; LAUNCH_LOG=""; LOAD_CPU_DIR=""; PAUSE_PID=""
# Waiting on a child with bash's wait builtin lets TERM/INT interrupt a long
# measurement sleep immediately; cleanup also reaps that child.
tiered_pause() {
    sleep "$1" &
    PAUSE_PID=$!
    wait "$PAUSE_PID"
    PAUSE_PID=""
}
cleanup_rate() {
    local failed=0 unit loaded
    if [[ -n "$PAUSE_PID" ]]; then
        kill "$PAUSE_PID" 2>/dev/null || :
        wait "$PAUSE_PID" 2>/dev/null || :
        PAUSE_PID=""
    fi
    for unit in "$TRACER_UNIT" "$LOAD_SCOPE"; do
        [[ -z "$unit" ]] || systemctl stop "$unit" >/dev/null 2>&1 || :
    done
    [[ -z "$LOAD_LAUNCH" ]] || wait "$LOAD_LAUNCH" 2>/dev/null || :
    for unit in "$TRACER_UNIT" "$LOAD_SCOPE"; do
        [[ -z "$unit" ]] && continue
        if ! loaded=$(systemctl list-units --all --no-legend --plain "$unit"); then
            echo "ERROR: cannot verify cleanup of $unit"
            failed=1
        elif [[ -n "$loaded" ]]; then
            echo "ERROR: cleanup left unit $unit loaded"
            failed=1
        else
            echo "  CLEANUP unit not loaded: $unit"
        fi
    done
    if [[ -n "$LOAD_CPU_DIR" ]]; then
        if rmdir "$LOAD_CPU_DIR"; then
            echo "  CLEANUP removed $LOAD_CPU_DIR"
        else
            echo "ERROR: cleanup left CPU group $LOAD_CPU_DIR"
            failed=1
        fi
    fi
    [[ -z "$LOG" || ! -f "$LOG" ]] || { echo "  daemon startup/log:"; cat "$LOG"; }
    [[ -z "$LAUNCH_LOG" || ! -f "$LAUNCH_LOG" ]] || cat "$LAUNCH_LOG"
    [[ -z "$TRACE_DIR" ]] || rm -rf "$TRACE_DIR"
    [[ -z "$LOG" ]] || rm -f "$LOG"
    [[ -z "$LAUNCH_LOG" ]] || rm -f "$LAUNCH_LOG"
    TRACER_UNIT=""; LOAD_SCOPE=""; LOAD_LAUNCH=""
    TRACE_DIR=""; LOG=""; LAUNCH_LOG=""; LOAD_CPU_DIR=""
    return "$failed"
}
trap 'rc=$?; cleanup_rate || rc=1; exit "$rc"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

while [[ $# -gt 0 ]]; do
    case "$1" in
        --pid) PM_PID="$2"; shift 2 ;;
        --rates) RATES="$2"; shift 2 ;;
        --esc-seconds) ESC_SECONDS="$2"; shift 2 ;;
        --clients) CLIENTS="$2"; shift 2 ;;
        --tolerance) TOLERANCE="$2"; TOLERANCE_OVERRIDDEN=1; shift 2 ;;
        *) echo "Usage: $0 [--pid PID] [--rates \"10 50 100\"] [--esc-seconds N] [--clients N]"; exit 1 ;;
    esac
done

[[ -z "$PM_PID" ]] && PM_PID=$(find_postmaster)
if [[ -z "$PM_PID" ]]; then echo "ERROR: cannot find postmaster PID"; exit 1; fi
if [[ ! -x "$CPU_GROUP_READER" ]]; then echo "ERROR: build tests/cpu_group_readback first"; exit 1; fi
if [[ ! -x "$XVAL" ]]; then echo "ERROR: build tests/cross_validate first (make -C tests cross_validate)"; exit 1; fi

echo "=== test_cross_validate_tiered (PID $PM_PID) ==="
echo "Rates: $RATES   escalation: ${ESC_SECONDS}s   clients: $CLIENTS"

# The tolerance this run gates on must be the DERIVED one, not whatever the
# constant above happens to say. `make check` already enforces that on the
# Mac; this re-checks it here because the gate box builds from an rsync'd
# tree and the whole point of the derivation is that the number cannot drift
# away from it unnoticed. An explicit --tolerance is a human override and is
# allowed, but it is announced, so a log can never be read as evidence about
# the derived bound when it was not run against it.
if [[ "$TOLERANCE_OVERRIDDEN" -eq 1 ]]; then
    echo "NOTE: tolerance OVERRIDDEN on the command line to +/-${TOLERANCE}pp;" \
         "this run is NOT evidence about the derived bound" \
         "($(derive_xval_tolerance "$XVAL_PREREG_BIAS_PP" "$XVAL_PREREG_SAMPLES" "$XVAL_PREREG_K" 2>/dev/null || echo '?') pp)."
# $SCRIPT_DIR + basename, not a bare ${BASH_SOURCE[0]}: run_all.sh happens to
# invoke this with an absolute path, so BASH_SOURCE works there, but a caller
# that passed a relative path AND changed directory would make the guard
# unable to read its own file -- and an unreadable file is a REFUSAL, i.e. a
# spurious gate failure for an invocation style that is otherwise fine.
# SCRIPT_DIR is already absolutised at the top of this file.
elif ! DERIVED_TOL=$(xval_tolerance_matches_script "$SCRIPT_DIR/$(basename "${BASH_SOURCE[0]}")"); then
    echo "ERROR: this script's TOLERANCE does not match the pre-registered"
    echo "       derivation in tests/xval_tolerance.sh (see the message above)."
    echo "       Refusing to run: a gate whose threshold it cannot justify"
    echo "       would certify whatever it happens to measure."
    exit 1
else
    echo "Tolerance: +/-${TOLERANCE}pp, derived (BIAS ${XVAL_PREREG_BIAS_PP} pp + z(${XVAL_PREREG_K})*SE at N=${XVAL_PREREG_SAMPLES}) — see tests/xval_tolerance.sh"
fi

ctl() {
    python3 - "$1" "$2" <<'PYEOF'
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.settimeout(5)
s.connect(sys.argv[1]); s.sendall((sys.argv[2] + "\n").encode())
buf=b""
while b"\n" not in buf:
    c=s.recv(4096)
    if not c: break
    buf+=c
sys.stdout.write(buf.decode().split("\n")[0])
PYEOF
}

# Issue #133: this used to run `pgbench -i -s 10` unconditionally on every
# invocation to "ensure the schema exists" -- harmless only because 10
# happened to equal tests/provision-runner.sh's provisioned scale; the
# moment that scale grows, this would silently shrink the shared dataset on
# every run_all, and even at a matching scale `-i` unconditionally drops and
# recreates every pgbench table (stats, bloat, cache state) out from under
# every other test. This test only needs pgbench's normal read/write
# workload against SOME populated pgbench schema -- the row count doesn't
# matter to sampled-vs-exact cross-validation -- so it reads the provisioned
# dataset read-only instead of ever re-initializing it.
ROWS=$(psql -U postgres -d postgres -tAc \
    "SELECT count(*) FROM pgbench_accounts" 2>/dev/null || echo 0)
if [[ "${ROWS:-0}" -lt 1 ]]; then
    echo "ERROR: pgbench_accounts is missing/empty in the shared 'postgres' database."
    echo "       Issue #133: this test reads the provisioned dataset read-only and"
    echo "       never re-initializes it. Run tests/provision-runner.sh (or"
    echo "       'pgbench -i -s 10 -d postgres') to seed it first."
    exit 1
fi

best_rate=""
declare -A rate_result
declare -A rate_delta
declare -A rate_event
declare -A rate_assertions
declare -A rate_invalid
non_tolerance_failure=0

for RATE in $RATES; do
    echo ""
    echo "----- sample-rate ${RATE} Hz -----"
    TRACE_DIR=$(mktemp -d /tmp/pgwt_xval_XXXXXX)
    SOCK="$TRACE_DIR/pgwt.sock"
    LOG=$(mktemp /tmp/pgwt_xval_log_XXXXXX)

    LAUNCH_LOG=$(mktemp /tmp/pgwt_xval_launch_XXXXXX)
    TRACER_UNIT="pgwt-xval-$$-${RATE}.service"
    LOAD_SCOPE="pgwt-xval-load-$$-${RATE}.scope"
    PGB_LOG="$TRACE_DIR/pgbench.log"
    APP_NAME="pgwt-xval-$$-${RATE}"

    # Establish the load placement BEFORE the plain service and keep it alive.
    # v2: the load scope's default CPU weight enables cpu outside system.slice.
    # v1: a private CPU cgroup avoids systemd's lazy realization of sibling
    # service groups. Giving a systemd unit a CPU property can move services
    # between / and /system.slice as its hierarchy is realized/trimmed.
    # The tracer itself never chooses a CPU, Nice or Slice property.
    LOAD_PROPERTIES=()
    PM_GROUP=$("$CPU_GROUP_READER" "$PM_PID") || { echo "INVALID layout: $PM_GROUP (exit 80)"; exit 80; }
    if [[ "$PM_GROUP" == v1$'\t'* ]]; then
        CPU_MOUNT=$(python3 - <<'PYEOF'
from pathlib import Path
import re
for line in Path("/proc/self/mountinfo").read_text().splitlines():
    before, after = line.split(" - ", 1)
    fields = after.split()
    if fields[0] == "cgroup" and "cpu" in fields[2].split(","):
        print(re.sub(r"\\([0-7]{3})", lambda m: chr(int(m[1], 8)), before.split()[4]))
        break
else:
    raise SystemExit("CPU mount unavailable")
PYEOF
) || { echo "INVALID layout: cannot find v1 CPU mount (exit 80)"; exit 80; }
        LOAD_CPU_DIR="$CPU_MOUNT/pgwt-xval-load-$$-${RATE}"
        mkdir "$LOAD_CPU_DIR" || { LOAD_CPU_DIR=""; echo "INVALID layout: cannot create load CPU group (exit 80)"; exit 80; }
    else
        LOAD_PROPERTIES=(-p CPUWeight=100)
    fi
    systemd-run --quiet --scope --collect --unit="$LOAD_SCOPE" --slice=user.slice \
        "${LOAD_PROPERTIES[@]}" /bin/bash -c '
        if [[ -n "$5" ]]; then echo "$$" > "$5/cgroup.procs" || exit 1; fi
        echo "$$" > "$1/pgbench.pid"
        for ((i=0; i<300; i++)); do
            if [[ -f "$1/load-go" ]]; then
                exec env PGAPPNAME="$4" pgbench -U postgres -d postgres \
                    -c "$2" -j 2 -T "$3" -P 5 --progress-timestamp
            fi
            sleep 0.1
        done
        exit 1' -- "$TRACE_DIR" "$CLIENTS" "$((ESC_SECONDS + 8))" "$APP_NAME" "$LOAD_CPU_DIR" \
        >"$PGB_LOG" 2>&1 &
    LOAD_LAUNCH=$!
    for _ in $(seq 1 50); do [[ -s "$TRACE_DIR/pgbench.pid" ]] && break; sleep 0.1; done
    if [[ ! -s "$TRACE_DIR/pgbench.pid" ]]; then
        echo "INVALID layout: load scope did not start (exit 80)"; cat "$PGB_LOG"; exit 80
    fi
    PGB=$(cat "$TRACE_DIR/pgbench.pid")

    # system services do not inherit the caller's environment. Forward the
    # exported PostgreSQL/tracer settings, including an explicit nice override.
    TRACER_ENV=()
    for name in ${!PG@}; do TRACER_ENV+=("$name=${!name}"); done
    # Start synchronously: a signal trap runs only after systemd-run returns,
    # so stop cannot race creation of the daemon's unit. ExecStopPost records
    # the real daemon exit before --collect removes the unit.
    systemd-run --quiet --collect --unit="$TRACER_UNIT" \
        -p StandardOutput=null -p "StandardError=file:$LOG" \
        -p "ExecStopPost=/bin/sh \"$SCRIPT_DIR/xval_daemon_exit.sh\" \"$TRACE_DIR/daemon.exit\"" \
        /usr/bin/env "${TRACER_ENV[@]}" "$TRACER" --daemon --pid "$PM_PID" \
        -i 1 -T "$TRACE_DIR" --mode tiered --sample-rate "$RATE" \
        --escalation-budget 600 -q >"$LAUNCH_LOG" 2>&1 || non_tolerance_failure=1
    for _ in $(seq 1 30); do [[ -S "$SOCK" ]] && break; sleep 0.3; done
    if [[ ! -S "$SOCK" ]]; then
        echo "  daemon failed to start"; non_tolerance_failure=1
        cleanup_rate || non_tolerance_failure=1
        continue
    fi
    TPID=$(systemctl show "$TRACER_UNIT" -p MainPID --value)
    touch "$TRACE_DIR/load-go"
    tiered_pause 3   # warm up the sampler before escalating

    # Escalate for the window
    ESC_REQUEST=$(python3 -c 'import time; print(time.time())')
    RESP=$(ctl "$SOCK" "{\"cmd\":\"escalate\",\"duration_s\":${ESC_SECONDS},\"reason\":\"cross-validate\"}") || non_tolerance_failure=1
    ESC_START=$(python3 -c 'import time; print(time.time())')
    ESC_END=$(python3 -c 'import sys; print(float(sys.argv[1]) + int(sys.argv[2]))' "$ESC_REQUEST" "$ESC_SECONDS")
    echo "  escalate: $RESP"

    # Read real placement during the workload, after escalation has begun.
    tiered_pause 5
    BACKEND_PID=$(psql -U postgres -d postgres -tAc \
        "SELECT pid FROM pg_stat_activity WHERE application_name='$APP_NAME' AND backend_type='client backend' ORDER BY pid LIMIT 1" 2>/dev/null) || BACKEND_PID=""
    tiered_layout_readback "$TPID" "$BACKEND_PID" "$PGB" || exit 80
    tiered_pause $((ESC_SECONDS - 2))   # remainder of window + expiry

    # Stop workload + daemon (flushes trace)
    systemctl stop "$LOAD_SCOPE" >/dev/null 2>&1
    PGB_RC=0; wait "$LOAD_LAUNCH" 2>/dev/null || PGB_RC=$?
    LOAD_LAUNCH=""
    [[ "$PGB_RC" -eq 0 || "$PGB_RC" -eq 143 ]] || non_tolerance_failure=1
    systemctl stop "$TRACER_UNIT" >/dev/null 2>&1
    TRACER_EXIT=$(cat "$TRACE_DIR/daemon.exit" 2>/dev/null) || TRACER_EXIT=unknown
    # Preserve the former guard: exit 0 or SIGTERM is accepted; all other
    # exits (including an unreadable exit record) remain a test failure.
    [[ "$TRACER_EXIT" == exited:0 || "$TRACER_EXIT" == killed:TERM ||
       "$TRACER_EXIT" == killed:15 ]] || non_tolerance_failure=1

    # Compare
    XVAL_RC=0
    OUT=$("$XVAL" "$TRACE_DIR" --tolerance "$TOLERANCE") || XVAL_RC=$?
    echo "  pgbench output (progress uses Unix epoch timestamps):"
    cat "$PGB_LOG"
    VALIDITY=$(tiered_workload_validity "$OUT" "$PGB_LOG" "$ESC_START" "$ESC_END") || VALIDITY="b': validity checker failed"
    if [[ "$VALIDITY" != VALID ]]; then
        rate_assertions[$RATE]=WORKLOAD_INVALID
        rate_invalid[$RATE]="$VALIDITY"
        echo "  RATE ${RATE}Hz: INVALID — $VALIDITY"
        echo "  assertions[$RATE]=WORKLOAD_INVALID xval_rc=$XVAL_RC"
        cleanup_rate || non_tolerance_failure=1
        continue
    fi
    python3 - "$PGB_LOG" "$ESC_START" "$ESC_END" "$RATE" <<'PYEOF'
from pathlib import Path
import re
import sys
log, start, end, rate = sys.argv[1:]
intervals = []
for line in Path(log).read_text().splitlines():
    m = re.match(r"progress: ([0-9.]+)(?: s)?, ([0-9.]+) tps,", line)
    if m and float(start) <= float(m[1]) - 5 and float(m[1]) <= float(end):
        intervals.append(float(m[2]))
print(f"  PGBENCH_TPS {rate}Hz: {sum(intervals)/len(intervals):.2f} (mean complete 5 s intervals in escalation)")
PYEOF
    DELTA_INFO=$(extract_max_delta "$OUT")
    rate_delta[$RATE]="${DELTA_INFO%%|*}"
    rate_event[$RATE]="${DELTA_INFO#*|}"

    # Did the comparator see enough of a capture for the derived bound to
    # mean anything? Two distinct refusals, both leaving rate_result UNSET
    # so the summary prints NO RESULT and tiered_classify sees an INVALID arm
    # (the #309 pass-by-omission rule):
    #
    #  1. N absent or unparseable -- the gate is blind. A blind gate must
    #     refuse, never approve; this is the same disposition
    #     extract_max_delta already takes for a missing delta line.
    #  2. N below the DERIVED floor (derive_xval_sample_floor, 2587), which
    #     marks a structurally broken capture -- stalled daemon, mis-aligned
    #     window, dead workload -- rather than ordinary variation. It sits
    #     4.4 sd below the mean of this harness's own capture-thickness
    #     distribution (mean 3102, sd 117, n=11), so ordinary variation
    #     cannot trip it; see the derivation in tests/xval_tolerance.sh.
    #
    #     ROUND 1 GOT THIS WRONG and the correction is the point: the floor
    #     was XVAL_PREREG_SAMPLES (3130), the minimum of a prior n=5 taken
    #     on a DIFFERENT harness, and it fired on 5 of 10 ordinary captures
    #     -- a false failure, not a detection. The tolerance's N and the
    #     validity floor are two different quantities and are now two
    #     different numbers.
    #
    #     Applied at EVERY rate: at 50/100/200 Hz the observed N is
    #     15,718 / 30,527 / 62,864, so the floor cannot cause a false
    #     failure there, but it still catches a broken capture -- which the
    #     previous "any N >= 1" rule at those rates did not.
    SAMP=$(extract_samples_in_window "$OUT")
    if ! SAMP_MIN=$(derive_xval_sample_floor); then
        non_tolerance_failure=1
        echo "  assertions[$RATE]=INVALID xval_rc=$XVAL_RC"
        echo "  RATE ${RATE}Hz: REFUSED — the capture-thickness floor could not be derived;" \
             "thickness cannot be judged, so this counts as a failure, not a skip"
        cleanup_rate || non_tolerance_failure=1
        continue
    fi
    if ! refuse_unless_samples_sufficient "$SAMP" "$SAMP_MIN" >/dev/null; then
        non_tolerance_failure=1
        echo "  assertions[$RATE]=INVALID xval_rc=$XVAL_RC"
        echo "  RATE ${RATE}Hz: REFUSED — capture too thin / sample count unreadable" \
             "(N=${SAMP}, need >=${SAMP_MIN}); counts as a failure, not a skip"
        cleanup_rate || non_tolerance_failure=1
        continue
    fi
    echo "  samples in window: $SAMP (>= $SAMP_MIN required for the derived bound)"
    rate_assertions[$RATE]=$(tiered_arm_assertions "$OUT" "$XVAL_RC" "$TOLERANCE") || rate_assertions[$RATE]=INVALID

    echo "  assertions[$RATE]=${rate_assertions[$RATE]} xval_rc=$XVAL_RC"
    if [[ "${rate_assertions[$RATE]}" == INVALID ]]; then
        non_tolerance_failure=1
        echo "  RATE ${RATE}Hz: INVALID — comparator assertions: assertions=INVALID xval_rc=$XVAL_RC"
    elif [[ "$OUT" == *"RESULT: PASS"* ]]; then
        rate_result[$RATE]="PASS"
        [[ -z "$best_rate" ]] && best_rate="$RATE"
    else
        rate_result[$RATE]="FAIL"
    fi

    [[ "${rate_assertions[$RATE]}" == INVALID ]] || print_rate_result "$RATE" "$OUT"
    cleanup_rate || non_tolerance_failure=1
done

echo ""
echo "===== Cross-validation summary ====="
# Collect EVERY requested rate: a daemon/startup or capture-validity failure
# leaves its assertions unset, so INVALID makes the final decision exit 1.
# This preserves #309's guard against passing by omission.
ARM_ASSERTIONS=()
DEVIATIONS=()
for RATE in $RATES; do
    if [[ "${rate_assertions[$RATE]:-}" == WORKLOAD_INVALID ]]; then
        echo "  RATE ${RATE}Hz: INVALID — ${rate_invalid[$RATE]}"
    else
        print_rate_summary "$RATE" "${rate_result[$RATE]:-}" "${rate_delta[$RATE]:-?}" \
            "${rate_event[$RATE]:-n/a}" "$TOLERANCE"
    fi
    ARM_ASSERTIONS+=("${rate_assertions[$RATE]:-INVALID}")
    DEVIATIONS+=("${RATE}=${rate_delta[$RATE]:-?}")
done
echo "TIERED_DEVIATIONS ${DEVIATIONS[*]}"

if [[ -n "$best_rate" ]]; then
    echo "First rate within +/-${TOLERANCE}pp tolerance: ${best_rate} Hz"
fi

RESULT_RC=0
tiered_classify "$non_tolerance_failure" "$TOLERANCE" ${ARM_ASSERTIONS[@]+"${ARM_ASSERTIONS[@]}"} || RESULT_RC=$?
if [[ "$RESULT_RC" -eq 0 ]]; then
    echo "PASS: every requested rate ($RATES) is within +/-${TOLERANCE}pp tolerance."
    exit 0
else
    echo "FAIL: not every requested rate is within +/-${TOLERANCE}pp tolerance — see per-rate lines above for which rate(s) and by how much."
    if [[ "$RESULT_RC" -eq 80 ]]; then
        echo "INVALID workload: see per-rate validity rules above (exit 80)."
        exit 80
    fi
    if [[ "$RESULT_RC" -eq 79 ]]; then
        echo "#675 signature only: sampled under-reads exact, all deviations <=15.0 pp (exit 79)."
        exit 79
    fi
    exit 1
fi
