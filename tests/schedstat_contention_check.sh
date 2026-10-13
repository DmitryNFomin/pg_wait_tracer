#!/bin/bash
# schedstat_contention_check.sh -- proves that the run_delay reading behind
# the Off-CPU* decomposition (issue #386, tests/schedstat_probe.py) actually
# MOVES with run-queue contention, and refuses when it cannot see.
#
# A number you cannot move is a number you cannot trust: the whole point of
# subtracting Sigma run_delay from Off-CPU* is that run_delay measures
# runnable-but-not-running time INDEPENDENTLY of this project's own
# instrumentation. If the reading were stuck -- and by default it IS stuck,
# because kernel.sched_schedstats is 0 on both gate boxes -- the
# subtraction would silently credit the entire residual to "unexplained".
#
# Three legs, in this order, on the box (Linux only):
#   1. REFUSAL   with kernel.sched_schedstats=0, the probe must exit 2 and
#                report a refusal, NOT a tidy 0 ms.
#   2. IDLE      schedstats on, N sleeping children: Sigma run_delay must be
#                near zero (they are never runnable-but-waiting).
#   3. CONTENDED schedstats on, 4x nproc spinning children: Sigma run_delay
#                must rise by orders of magnitude.
#
# Restores the original kernel.sched_schedstats value on exit, whatever
# happens (it is a global sysctl, and leaving it flipped on a shared box
# would change every later measurement taken there).
#
# CONCURRENCY: kernel.sched_schedstats is GLOBAL, so two runs on one box --
# this script and a rehearsal, or two rehearsals -- can have the first
# teardown reset it under the second. The consequence is refusals, never
# wrong data (tests/schedstat_lib.py requires the sysctl to have been 1 for
# the WHOLE window and refuses otherwise), so the fail-safe holds; but take
# /tmp/pgwt-box-check.lock around this script on a shared box, as the run
# recorded below did, rather than relying on that.
#
# NOT WIRED INTO A RUNNER, deliberately: it needs root, a global sysctl flip
# and a quiet box. It is a one-shot instrument, re-run by hand whenever
# schedstat_probe.py's enumeration or field parsing changes.
# RECORDED RUN: tests/results/schedstat_contention/ (2026-10-13, pgwt-gate-2,
# nproc=4) -- refusal at schedstats=0, 0.0 ms idle, 111,775.654 ms contended,
# 111,776x. See that directory's VERDICT.txt for what it does and does not
# establish.
#
# Usage (as root, on a Linux box):
#   tests/schedstat_contention_check.sh [SECONDS_PER_LEG] [OUT_DIR]
set -uo pipefail

DUR="${1:-10}"
OUT_DIR="${2:-tests/results/schedstat_contention}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

if [[ ! -r /proc/sys/kernel/sched_schedstats ]]; then
    echo "SKIP-REFUSE: /proc/sys/kernel/sched_schedstats is not readable -- this"
    echo "kernel has no CONFIG_SCHEDSTATS, so run_delay cannot be measured here"
    echo "at all. Reported as a refusal (exit 3), never as 'contention is zero'."
    exit 3
fi

ORIG_SCHEDSTATS="$(cat /proc/sys/kernel/sched_schedstats)"
mkdir -p "$OUT_DIR"

cleanup() {
    [[ -n "${SPIN_PID:-}" ]] && kill -- -"$SPIN_PID" 2>/dev/null
    sysctl -q -w "kernel.sched_schedstats=$ORIG_SCHEDSTATS" 2>/dev/null \
        || echo "WARNING: could not restore kernel.sched_schedstats=$ORIG_SCHEDSTATS" >&2
    echo "schedstat_contention: restored kernel.sched_schedstats=$(cat /proc/sys/kernel/sched_schedstats)"
}
trap cleanup EXIT

# A parent whose children are the task set the probe enumerates -- the same
# shape as postmaster + backends, so the enumeration path under test is the
# one the rehearsal uses, not a special case.
LOADGEN="$(mktemp /tmp/pgwt_spin_XXXXXX.sh)"
cat >"$LOADGEN" <<'SPIN'
#!/bin/bash
n="$1"; dur="$2"; mode="$3"
for _ in $(seq 1 "$n"); do
    if [[ "$mode" == "busy" ]]; then
        ( end=$((SECONDS + dur)); while [[ $SECONDS -lt $end ]]; do :; done ) &
    else
        sleep "$dur" &
    fi
done
wait
SPIN
chmod +x "$LOADGEN"

run_leg() {
    local label="$1" nchild="$2" mode="$3" schedstats="$4"
    sysctl -q -w "kernel.sched_schedstats=$schedstats"
    setsid "$LOADGEN" "$nchild" "$((DUR + 5))" "$mode" &
    SPIN_PID=$!
    sleep 1
    python3 "$SCRIPT_DIR/schedstat_probe.py" --parent-pid "$SPIN_PID" \
        --duration "$DUR" --interval 1 --label "$label" \
        >"$OUT_DIR/$label.json"
    local rc=$?
    kill -- -"$SPIN_PID" 2>/dev/null
    wait "$SPIN_PID" 2>/dev/null
    SPIN_PID=""
    echo "--- leg $label (children=$nchild mode=$mode sched_schedstats=$schedstats) probe rc=$rc"
    python3 - "$OUT_DIR/$label.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
rd = d["run_delay"]
print(f"    samples={d['samples']} refused={rd.get('refused')} "
      f"run_delay_ms={rd.get('run_delay_ms')} reason={rd.get('reason')}")
print(f"    pids={rd.get('pid_counts')} max_runnable={d['load'].get('max_runnable')} "
      f"mean_load1={d['load'].get('mean_load1')}")
PY
    return $rc
}

NCPU="$(nproc)"
echo "=== schedstat contention check (nproc=$NCPU, ${DUR}s per leg, "\
"original kernel.sched_schedstats=$ORIG_SCHEDSTATS) ==="

run_leg "disabled" "$NCPU" busy 0
DISABLED_RC=$?
run_leg "idle" "$NCPU" sleep 1
run_leg "contended" "$((NCPU * 4))" busy 1

rm -f "$LOADGEN"

python3 - "$OUT_DIR" "$DISABLED_RC" <<'PY'
import json, os, sys
out, disabled_rc = sys.argv[1], int(sys.argv[2])
def rd(name):
    return json.load(open(os.path.join(out, name + ".json")))["run_delay"]
disabled, idle, contended = rd("disabled"), rd("idle"), rd("contended")
ok = True
# Leg 1: the refusal must be a refusal, with a nonzero exit code.
if not (disabled.get("refused") and disabled.get("run_delay_ms") is None
        and disabled_rc == 2):
    ok = False
    print(f"FAIL leg 1: schedstats=0 did not refuse "
          f"(rc={disabled_rc}, {disabled})")
else:
    print(f"PASS leg 1: schedstats=0 REFUSED (probe rc=2): {disabled['reason'][:90]}")
# Legs 2/3: the reading must be near zero idle and must RISE under
# contention. The bar is 100x rather than a tuned threshold: 4x nproc
# spinners spend ~75% of their wall time runnable-but-waiting, which is
# seconds of run_delay per task, against milliseconds at idle.
for name, d in (("idle", idle), ("contended", contended)):
    if d.get("run_delay_ms") is None:
        ok = False
        print(f"FAIL leg {name}: refused unexpectedly: {d.get('reason')}")
if ok:
    i, c = idle["run_delay_ms"], contended["run_delay_ms"]
    print(f"idle Sigma run_delay   = {i:.1f} ms")
    print(f"contended Sigma run_delay = {c:.1f} ms")
    if c < 100 * max(i, 1.0):
        ok = False
        print(f"FAIL: contended/idle ratio {c / max(i, 1.0):.1f}x is under 100x "
              f"-- the reading did not move the way run-queue delay must")
    else:
        print(f"PASS: the reading MOVES -- {c / max(i, 1.0):.0f}x idle")
print("VERDICT:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
PY
rc=$?
echo "schedstat_contention: exit $rc (artifacts in $OUT_DIR)"
exit $rc
