#!/usr/bin/env bash
# repro_issue202.sh — the #202 reproduction matrix, on a live box.
#
# #202 was an intermittent 20.2% over-attribution in test_multi_window's
# Test-3 conservation check (class rows 18330 ms vs DB Time 15255 ms) on the
# persistent gate box. An intermittent failure needs a RATE, not a run, and
# the rate depends on how CPU-starved the box is — so this drives the real
# test N times under three conditions and prints how often it failed.
#
#   MODE=harness   tests/repro_issue202.py: per-window excess AND the
#                  measured off-CPU deficit / clock-skew underflow counts
#                  behind it (PGWT_DEBUG_DUMP_STATE). Use this to attribute
#                  an excess, not just to observe one.
#   MODE=test      the real tests/test_multi_window.py, N times, FAIL rate.
#
#   LOAD=<n>       n busy-loop processes (CPU starvation).
#   CPUQ=1         also park 3 long pure-CPU backends, so a we==0 state_map
#                  stretch of SECONDS is open at every window boundary —
#                  the condition mechanism A needs to be large.
#
# Usage (as root, on a box with PostgreSQL running):
#   PGWT_PM_PID=<postmaster> PGPORT=<port> \
#     MODE=test LOAD=8 CPUQ=1 tests/repro_issue202.sh <tree> <label> [n]
#
# Rates measured on a cx33 gate-snapshot VM, PG18, tree = master@b8c5e31:
#   MODE=harness LOAD=0            0 of 6 windows over the 2% threshold
#   MODE=harness LOAD=8            0 of 6
#   MODE=test    LOAD=8            0 of 3 runs
#   MODE=test    LOAD=8 CPUQ=1     0 of 3 runs
# i.e. NOT reproducible on a private VM; the gate box stands at 1 of 2.
set -uo pipefail
TREE="${1:?usage: repro_issue202.sh <tree> <label> [n]}"
LABEL="${2:?}"; N="${3:-3}"
MODE="${MODE:-test}"; LOAD="${LOAD:-0}"; CPUQ="${CPUQ:-0}"
: "${PGWT_PM_PID:?set PGWT_PM_PID to the postmaster being traced}"
export PGPORT="${PGPORT:-5432}"

pids=()
cleanup(){
  for p in "${pids[@]:-}"; do kill "$p" 2>/dev/null || true; done
  [[ "$CPUQ" == "1" ]] && psql -U postgres -d postgres -c \
    "SELECT pg_terminate_backend(pid) FROM pg_stat_activity
      WHERE pid <> pg_backend_pid() AND query LIKE '%generate_series%'" \
    >/dev/null 2>&1
  return 0
}
trap cleanup EXIT

for _ in $(seq 1 "$LOAD"); do bash -c 'while :; do :; done' & pids+=($!); done
if [[ "$CPUQ" == "1" ]]; then
  for _ in 1 2 3; do
    psql -U postgres -d postgres -c \
      "SELECT count(*) FROM generate_series(1,4000000000)" >/dev/null 2>&1 &
    pids+=($!)
  done
fi
[[ "$LOAD" -gt 0 || "$CPUQ" == "1" ]] && sleep 3

echo "### $LABEL: mode=$MODE tree=$TREE load=$LOAD cpuq=$CPUQ n=$N" \
     "(loadavg $(cut -d' ' -f1-3 /proc/loadavg), $(nproc) vCPU)"

if [[ "$MODE" == "harness" ]]; then
  PGWT_TRACER="$TREE/pg_wait_tracer" python3 "$(dirname "$0")/repro_issue202.py" \
      -n "$N" --pid "$PGWT_PM_PID" --out "/tmp/repro202-$LABEL.json"
  exit $?
fi

pass=0; fail=0
for i in $(seq 1 "$N"); do
  out=$(cd "$TREE" && python3 tests/test_multi_window.py --pid "$PGWT_PM_PID" 2>&1)
  printf '%s\n' "$out" > "/tmp/mw-$LABEL-$i.log"
  line=$(printf '%s\n' "$out" | grep -E "DB Time consistency" | tail -1)
  echo "[$LABEL run $i]$line"
  # A run that produced no consistency line at all is a FAILURE, not a pass:
  # the check must never be counted green because it did not run.
  if [[ -z "$line" ]] || printf '%s' "$line" | grep -q "FAIL"; then
    fail=$((fail+1))
  else
    pass=$((pass+1))
  fi
done
echo "### $LABEL: Test-3 consistency FAILED $fail / $((pass+fail)) runs" \
     "(loadavg $(cut -d' ' -f1-3 /proc/loadavg))"
[[ "$fail" -eq 0 ]]
