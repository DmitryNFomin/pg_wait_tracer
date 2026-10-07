#!/bin/bash
# overhead294.sh -- daemon CPU cost of the sampled tier, base vs fix, n per cell.
#
# --mode sampled (the SHIPPED default tier, whose "~0.6% of one core" claim is
# what the #294 recheck could move), identical pgbench load, a 60 s steady-state
# window measured from /proc/<daemon>/stat utime+stime. Interleaves base and fix
# runs so a drift in box temperature/noise cannot land on one arm.
#
# Any run that cannot produce its measurement prints OVH-ERROR and contributes
# NOTHING -- a missing measurement is never a zero.
set -u
BASE=/root/pgwt-check/master-base
FIX=/root/pgwt-check/agent_sampler-cmd-gate-order
N="${N:-5}"
WIN="${WIN:-60}"
PGPORT_USE="${PGPORT_USE:-5418}"
CLIENTS="${CLIENTS:-8}"
OUT=/root/ovh294
mkdir -p $OUT

PM_PID=$(pgrep -f "bin/postgres -D /var/lib/postgresql/18/main" | head -1)
[[ -n "${PM_PID:-}" ]] || { echo "OVH-ERROR: no postmaster"; exit 1; }

cpu_ticks() {  # $1 = pid -> utime+stime
    [[ -r "/proc/$1/stat" ]] || { echo ""; return; }
    awk '{ n = index($0, ") "); split(substr($0, n + 2), a, " ");
           print a[12] + a[13] }' "/proc/$1/stat"
}

one_run() { # tree rate iter
    local tree="$1" rate="$2" it="$3"
    local repo=$BASE; [[ $tree == fix ]] && repo=$FIX
    local td=$OUT/$tree-$rate-$it
    rm -rf "$td"; mkdir -p "$td"
    local sock="$td/pgwt.sock"

    pgbench -h /var/run/postgresql -p "$PGPORT_USE" -U postgres -d postgres \
        -c "$CLIENTS" -j 2 -T $((WIN + 25)) > "$td/pgbench.txt" 2>&1 &
    local pgb=$!
    sleep 3

    "$repo/pg_wait_tracer" --daemon --pid "$PM_PID" -i 1 -T "$td" \
        --mode sampled --sample-rate "$rate" -q >/dev/null 2>"$td/daemon.log" &
    local tp=$!
    local i
    for i in $(seq 1 40); do [[ -S "$sock" ]] && break; sleep 0.3; done
    if [[ ! -S "$sock" ]]; then
        echo "OVH-ERROR $tree $rate $it: daemon never created its socket"
        kill $tp $pgb 2>/dev/null; wait 2>/dev/null; return
    fi
    sleep 5    # let attach/warmup settle before the measured window

    local t0 w0 t1 w1
    t0=$(cpu_ticks $tp); w0=$(date +%s%N)
    sleep "$WIN"
    t1=$(cpu_ticks $tp); w1=$(date +%s%N)
    local metrics
    metrics=$(python3 - "$sock" <<'PY'
import socket, sys
try:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.settimeout(10)
    s.connect(sys.argv[1]); s.sendall(b'{"cmd":"metrics"}\n')
    buf = b""
    while b"\n" not in buf:
        c = s.recv(65536)
        if not c: break
        buf += c
    print(buf.decode().split("\n")[0])
except Exception as e:
    print("")
PY
)
    kill -TERM $tp 2>/dev/null; wait $tp 2>/dev/null
    kill $pgb 2>/dev/null; wait $pgb 2>/dev/null

    if [[ -z "$t0" || -z "$t1" ]]; then
        echo "OVH-ERROR $tree $rate $it: daemon /proc unreadable at a boundary"
        return
    fi
    echo "$metrics" > "$td/metrics.json"
    python3 - "$tree" "$rate" "$it" "$t0" "$t1" "$w0" "$w1" \
        "$td/metrics.json" <<'PY'
import json, os, sys
tree, rate, it, t0, t1, w0, w1, mf = sys.argv[1:9]
hz = os.sysconf("SC_CLK_TCK")
ticks = int(t1) - int(t0)
wall = (int(w1) - int(w0)) / 1e9
cpu_ns = ticks * 1e9 / hz
try:
    m = json.load(open(mf))
except Exception:
    print("OVH-ERROR %s %s %s: metrics unparseable -- run discarded"
          % (tree, rate, it))
    sys.exit(0)
samples = m.get("samples_total")
if samples is None:
    print("OVH-ERROR %s %s %s: samples_total absent -- run discarded"
          % (tree, rate, it))
    sys.exit(0)
print("OVH %s rate=%s iter=%s ticks=%d wall_s=%.3f cpu_ns=%.0f "
      "pct_one_core=%.4f samples=%s at_risk=%s recovered=%s read_failed=%s"
      % (tree, rate, it, ticks, wall, cpu_ns, 100.0 * cpu_ns / 1e9 / wall,
         samples, m.get("cmd_gate_order_at_risk_total", "ABSENT"),
         m.get("cmd_gate_order_recovered_total", "ABSENT"),
         m.get("cmd_gate_order_read_failed_total", "ABSENT")))
PY
}

for rate in 10 200; do
  for it in $(seq 1 "$N"); do
    one_run base "$rate" "$it"
    one_run fix  "$rate" "$it"
  done
done
