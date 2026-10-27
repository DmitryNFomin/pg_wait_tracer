#!/usr/bin/env bash
# INSTRUMENTATION for issue #671 (scratch branch agent/671-tier-truth).
# NOT a gated test; registered nowhere; touches no src/ and no gated test.
#
# One run = one tiered capture at a fixed sample rate with an escalation
# window, retaining everything tests/test_cross_validate_tiered.sh throws
# away, plus a third-party ground truth G from /proc/<pid>/schedstat.
#
# G = sum over CLIENT BACKEND pids of delta(field 1 of /proc/<pid>/schedstat)
#     across the escalation window.  Field 1 (1-indexed, the FIRST field) is
#     "time spent on the cpu", in ns -- the on-CPU total the kernel itself
#     accounts, independent of both tiers.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"
source "$REPO/tests/testutil.sh"

LABEL=""; POLICY="other"; RATE=10; CLIENTS=8; ESC=300; OUTROOT=""
PGMAJOR=18
while [[ $# -gt 0 ]]; do
  case "$1" in
    --label) LABEL="$2"; shift 2 ;;
    --policy) POLICY="$2"; shift 2 ;;
    --rate) RATE="$2"; shift 2 ;;
    --clients) CLIENTS="$2"; shift 2 ;;
    --esc) ESC="$2"; shift 2 ;;
    --out) OUTROOT="$2"; shift 2 ;;
    --pg-version) PGMAJOR="$2"; shift 2 ;;
    *) echo "unknown arg $1"; exit 2 ;;
  esac
done
[[ -z "$LABEL" || -z "$OUTROOT" ]] && { echo "need --label and --out"; exit 2; }

OUT="$OUTROOT/$LABEL"
mkdir -p "$OUT"
exec > >(tee "$OUT/run.log") 2>&1

TRACER="$REPO/pg_wait_tracer"
XVAL="$REPO/tests/cross_validate"
[[ -x "$TRACER" ]] || { echo "FATAL: $TRACER missing"; exit 3; }
[[ -x "$XVAL" ]]   || { echo "FATAL: $XVAL missing (make -C tests cross_validate)"; exit 3; }
command -v pgbench >/dev/null || { echo "FATAL: pgbench not on PATH"; exit 3; }
command -v chrt    >/dev/null || { echo "FATAL: chrt not found"; exit 3; }

PM=$(find_postmaster --pg-version "$PGMAJOR")
[[ -n "$PM" ]] || { echo "FATAL: no PG $PGMAJOR postmaster"; exit 3; }
echo "label=$LABEL policy=$POLICY rate=$RATE clients=$CLIENTS esc=${ESC}s pm=$PM port=${PGPORT:-unset}"

# --- static host facts -------------------------------------------------
uname -r              > "$OUT/uname-r.txt"
lscpu                 > "$OUT/lscpu.txt"
cat /etc/os-release   > "$OUT/os-release.txt"
"$TRACER" --version   > "$OUT/tracer-version.txt" 2>&1 || true
git -C "$REPO" rev-parse HEAD > "$OUT/commit.txt" 2>/dev/null || echo unknown > "$OUT/commit.txt"

# --- refuse an idle / unseeded DB (the #678 vacuous-pass failure mode) --
ROWS=$(psql -U postgres -d postgres -tAc "SELECT count(*) FROM pgbench_accounts" 2>/dev/null || echo 0)
if [[ "${ROWS:-0}" -lt 1 ]]; then echo "FATAL: pgbench_accounts empty/missing"; exit 3; fi
echo "pgbench_accounts rows: $ROWS"

ctl() {
  python3 - "$1" "$2" <<'PYEOF'
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.settimeout(10)
s.connect(sys.argv[1]); s.sendall((sys.argv[2] + "\n").encode())
buf=b""
while b"\n" not in buf:
    c=s.recv(65536)
    if not c: break
    buf+=c
sys.stdout.write(buf.decode().split("\n")[0])
PYEOF
}

client_backends() {
  psql -U postgres -d postgres -tAc \
    "SELECT pid FROM pg_stat_activity WHERE backend_type='client backend' AND pid<>pg_backend_pid()" \
    2>/dev/null
}
schedstat_snapshot() {   # $1 = output file; writes "pid field1_ns"
  local f="$1"; : > "$f"
  local p
  for p in $(client_backends); do
    local v; v=$(awk '{print $1}' "/proc/$p/schedstat" 2>/dev/null) || continue
    [[ -n "$v" ]] && echo "$p $v" >> "$f"
  done
}
cpu_stat_line() { awk '/^cpu /{print}' /proc/stat; }

TRACE_DIR=$(mktemp -d /tmp/pgwt671_XXXXXX)
SOCK="$TRACE_DIR/pgwt.sock"

# --- start the daemon, under the requested policy ----------------------
if [[ "$POLICY" == "fifo" ]]; then
  chrt -f 10 "$TRACER" --daemon --pid "$PM" -i 1 -T "$TRACE_DIR" \
      --mode tiered --sample-rate "$RATE" --escalation-budget 900 -q \
      >/dev/null 2>"$OUT/daemon.err" &
else
  "$TRACER" --daemon --pid "$PM" -i 1 -T "$TRACE_DIR" \
      --mode tiered --sample-rate "$RATE" --escalation-budget 900 -q \
      >/dev/null 2>"$OUT/daemon.err" &
fi
TPID=$!
for _ in $(seq 1 40); do [[ -S "$SOCK" ]] && break; sleep 0.3; done
if [[ ! -S "$SOCK" ]]; then
  echo "FATAL: daemon socket never appeared"; tail -20 "$OUT/daemon.err"
  kill "$TPID" 2>/dev/null; rm -rf "$TRACE_DIR"; exit 4
fi

# --- POLICY READ-BACK: refuse, do not silently fall through ------------
# /proc/<pid>/stat field 41 is the scheduling policy (0=SCHED_OTHER,
# 1=SCHED_FIFO).  Read the main thread AND every task: chrt applies at
# exec and threads inherit, so a thread on a different policy is a real
# finding, not noise.  A failed `chrt` reading as "hypothesis disproved"
# already happened once in this project; this block makes it impossible.
polfile="$OUT/policy_readback.txt"
: > "$polfile"
for t in /proc/$TPID/task/*; do
  tid=$(basename "$t")
  # strip "comm" (may contain spaces/parens) before field-counting
  rest=$(sed 's/.*) //' "/proc/$tid/stat" 2>/dev/null) || continue
  # after stripping "pid (comm) ", field 1 is state => stat field 41 is $39 here
  pol=$(echo "$rest" | awk '{print $39}')
  prio=$(echo "$rest" | awk '{print $38}')
  echo "tid=$tid policy=$pol rtprio=$prio" >> "$polfile"
done
MAINPOL=$(/usr/bin/grep -m1 "^tid=$TPID " "$polfile" | sed 's/.*policy=\([0-9]*\).*/\1/')
echo "policy readback: main tid $TPID -> $MAINPOL ($(wc -l < "$polfile") tasks)"
WANT=0; [[ "$POLICY" == "fifo" ]] && WANT=1
if [[ "$MAINPOL" != "$WANT" ]]; then
  echo "FATAL: policy readback mismatch: wanted $WANT (${POLICY}), got '$MAINPOL'."
  echo "       Refusing to record this run -- a silently failed chrt reads as"
  echo "       'hypothesis disproved'."
  kill -TERM "$TPID" 2>/dev/null; wait "$TPID" 2>/dev/null; rm -rf "$TRACE_DIR"; exit 5
fi

# --- workload ----------------------------------------------------------
pgbench -U postgres -d postgres -c "$CLIENTS" -j 2 -T $((ESC + 20)) \
    > "$OUT/pgbench.txt" 2>&1 &
PGB=$!
sleep 8   # warm up the sampler and let all $CLIENTS backends exist

# --- baseline counters + G start ---------------------------------------
ctl "$SOCK" '{"cmd":"metrics"}' > "$OUT/metrics_before.json"; echo
cpu_stat_line > "$OUT/procstat_before.txt"
cat /proc/loadavg > "$OUT/loadavg_before.txt"

RESP=$(ctl "$SOCK" "{\"cmd\":\"escalate\",\"duration_s\":${ESC},\"reason\":\"671\"}")
echo "escalate: $RESP"
T0_NS=$(date +%s%N)
schedstat_snapshot "$OUT/schedstat_t0.txt"
echo "G t0 pids: $(wc -l < "$OUT/schedstat_t0.txt")"

# churn watch: record the client-backend pid set every 15s
( for _ in $(seq 1 $((ESC/15 + 1))); do
    echo "$(date +%s) $(client_backends | tr '\n' ' ')" >> "$OUT/pidset_churn.txt"
    sleep 15
  done ) &
CHURN=$!

sleep "$ESC"
schedstat_snapshot "$OUT/schedstat_t1.txt"
T1_NS=$(date +%s%N)
echo "G t1 pids: $(wc -l < "$OUT/schedstat_t1.txt")"
kill "$CHURN" 2>/dev/null; wait "$CHURN" 2>/dev/null

cpu_stat_line > "$OUT/procstat_after.txt"
cat /proc/loadavg > "$OUT/loadavg_after.txt"
ctl "$SOCK" '{"cmd":"metrics"}' > "$OUT/metrics_after.json"; echo

sleep 4   # let the window expire cleanly
kill "$PGB" 2>/dev/null; wait "$PGB" 2>/dev/null
kill -TERM "$TPID" 2>/dev/null; wait "$TPID" 2>/dev/null

# --- the FULL comparator output, not the verdict line ------------------
"$XVAL" "$TRACE_DIR" --tolerance 8.8 --raw-ns --show-idle > "$OUT/xval.txt" 2>&1
XRC=$?
echo "cross_validate rc=$XRC"
du -sh "$TRACE_DIR" > "$OUT/trace_size.txt" 2>&1
ls -l "$TRACE_DIR" >> "$OUT/trace_size.txt" 2>&1
rm -rf "$TRACE_DIR"

# --- derive G, steal, bias ratio ---------------------------------------
python3 "$SCRIPT_DIR/derive.py" "$OUT" "$T0_NS" "$T1_NS" "$LABEL" "$POLICY" \
    > "$OUT/summary.txt" 2>&1
cat "$OUT/summary.txt"
