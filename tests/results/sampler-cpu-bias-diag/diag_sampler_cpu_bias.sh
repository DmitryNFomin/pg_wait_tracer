#!/bin/bash
# diag_sampler_cpu_bias.sh RATE OUTDIR [REPO]
#
# One tiered capture at RATE Hz with a 60 s escalation window under pgbench,
# keeping the trace dir. Collects, in this order (the order matters):
#   - /proc/<pid>/stat utime+stime for every PG client backend AND the daemon,
#     at window start and window end  (independent third reference)
#   - control-socket `metrics` BEFORE SIGTERM (the socket dies with the daemon)
#   - cross_validate --raw-ns on the preserved trace dir
# Every step that cannot produce its measurement prints DIAG-ERROR and the
# script exits nonzero: a missing measurement must never read as a zero.
set -uo pipefail

RATE="${1:?usage: $0 RATE OUTDIR [REPO]}"
OUT="${2:?usage: $0 RATE OUTDIR [REPO]}"
REPO="${3:-/root/pgwt-check/agent_sampler-cpu-bias-diag}"
ESC_SECONDS="${ESC_SECONDS:-60}"
CLIENTS="${CLIENTS:-8}"
PGPORT_USE="${PGPORT_USE:-5418}"

TRACER="$REPO/pg_wait_tracer"
XVAL="$REPO/tests/cross_validate"

die() { echo "DIAG-ERROR: $*" >&2; exit 1; }

[[ -x "$TRACER" ]] || die "no daemon at $TRACER"
[[ -x "$XVAL" ]]   || die "no cross_validate at $XVAL (make -C tests cross_validate)"
command -v pgbench >/dev/null || die "pgbench not on PATH (exit 127 case)"
command -v psql    >/dev/null || die "psql not on PATH (exit 127 case)"

mkdir -p "$OUT" || die "cannot create $OUT"
TRACE_DIR="$OUT/trace"
rm -rf "$TRACE_DIR"; mkdir -p "$TRACE_DIR"
SOCK="$TRACE_DIR/pgwt.sock"
LOG="$OUT/daemon.log"

PSQL="psql -h /var/run/postgresql -p $PGPORT_USE -U postgres -d postgres -tAc"
$PSQL "SELECT 1" >/dev/null 2>&1 || die "psql cannot reach PG on port $PGPORT_USE"
PM_PID=$(pgrep -f "bin/postgres -D /var/lib/postgresql/18/main" | head -1)
[[ -n "${PM_PID:-}" ]] || die "cannot resolve the PG18 postmaster pid"
ROWS=$($PSQL "SELECT count(*) FROM pgbench_accounts" 2>/dev/null || echo 0)
[[ "${ROWS:-0}" -ge 1 ]] || die "pgbench_accounts empty: the workload would be a no-op"

echo "DIAG rate=${RATE}Hz postmaster=$PM_PID pgbench_accounts=$ROWS out=$OUT"

ctl() {
    python3 - "$SOCK" "$1" <<'PYEOF'
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.settimeout(10)
s.connect(sys.argv[1]); s.sendall((sys.argv[2] + "\n").encode())
buf = b""
while b"\n" not in buf:
    c = s.recv(65536)
    if not c:
        break
    buf += c
sys.stdout.write(buf.decode().split("\n")[0])
PYEOF
}

# utime+stime in clock ticks for a pid list, plus CLOCK_MONOTONIC at read time.
snap_cpu() {
    local tag="$1"; shift
    local f="$OUT/proc-$tag.txt"
    : > "$f"
    printf 'MONO_NS %s\n' "$(date +%s%N)" >> "$f"
    for pid in "$@"; do
        [[ -r "/proc/$pid/stat" ]] || { echo "GONE $pid" >> "$f"; continue; }
        awk -v p="$pid" '{
            # fields 14,15 = utime,stime, counted from after comm (which may
            # contain spaces) -- split on ") " to keep the index honest.
            n = index($0, ") ");
            rest = substr($0, n + 2);
            split(rest, a, " ");
            # a[1] = state, so utime is a[12], stime a[13]
            printf "PID %s UTIME %s STIME %s SUM %s\n", p, a[12], a[13], a[12]+a[13]
        }' "/proc/$pid/stat" >> "$f"
    done
    echo "$f"
}

"$TRACER" --daemon --pid "$PM_PID" -i 1 -T "$TRACE_DIR" \
    --mode tiered --sample-rate "$RATE" --escalation-budget 600 -q \
    >/dev/null 2>"$LOG" &
TPID=$!
echo "$TPID" > "$OUT/daemon.pid"
for _ in $(seq 1 40); do [[ -S "$SOCK" ]] && break; sleep 0.3; done
[[ -S "$SOCK" ]] || { tail -20 "$LOG"; kill "$TPID" 2>/dev/null; die "daemon never created $SOCK"; }

ctl '{"cmd":"status"}'  > "$OUT/status-before.json" || die "status failed"
echo
grep -o '"sampled_attr_source":"[a-z_]*"' "$OUT/status-before.json" || \
    grep -o 'sampled_attr_source[^,]*' "$OUT/status-before.json" || \
    echo "DIAG-WARN: sampled_attr_source not in status"

pgbench -h /var/run/postgresql -p "$PGPORT_USE" -U postgres -d postgres \
    -c "$CLIENTS" -j 2 -T $((ESC_SECONDS + 10)) > "$OUT/pgbench.txt" 2>&1 &
PGB=$!
sleep 3

# Client-backend pids of THIS pgbench run (application_name is set by pgbench).
mapfile -t BACKENDS < <($PSQL "SELECT pid FROM pg_stat_activity WHERE backend_type='client backend' AND pid <> pg_backend_pid()")
[[ ${#BACKENDS[@]} -ge 1 ]] || die "no client backends visible: the workload is not running"
echo "DIAG client_backends=${#BACKENDS[@]} pids=${BACKENDS[*]}"

RESP=$(ctl "{\"cmd\":\"escalate\",\"duration_s\":${ESC_SECONDS},\"reason\":\"cpu-bias-diag\"}")
echo "DIAG escalate: $RESP"
[[ "$RESP" == *'"ok"'* || "$RESP" == *'escalat'* ]] || die "escalate refused: $RESP"

snap_cpu start "${BACKENDS[@]}" "$TPID" >/dev/null
sleep $((ESC_SECONDS - 2))
snap_cpu end "${BACKENDS[@]}" "$TPID" >/dev/null

# BEFORE TERM: the control socket disappears with the daemon.
ctl '{"cmd":"metrics"}' > "$OUT/metrics.json" || die "metrics read failed"
[[ -s "$OUT/metrics.json" ]] || die "metrics.json is empty -- counters unmeasured, not zero"
echo

sleep 3   # let the window expire
kill "$PGB" 2>/dev/null; wait "$PGB" 2>/dev/null
kill -TERM "$TPID" 2>/dev/null; wait "$TPID" 2>/dev/null

echo "DIAG pgbench: $(grep -E '^tps' "$OUT/pgbench.txt" | tr '\n' ' ')"

"$XVAL" "$TRACE_DIR" --tolerance 10 --raw-ns > "$OUT/xval.txt" 2>&1
XRC=$?
echo "DIAG cross_validate exit=$XRC"
cat "$OUT/xval.txt"

python3 - "$OUT" <<'PYEOF'
import json, os, sys, re
out = sys.argv[1]
def parse(tag):
    d = {}
    mono = None
    for line in open(os.path.join(out, "proc-%s.txt" % tag)):
        f = line.split()
        if f[0] == "MONO_NS":
            mono = int(f[1]); continue
        if f[0] == "GONE":
            d[int(f[1])] = None; continue
        d[int(f[1])] = int(f[7])
    return mono, d
m0, a = parse("start")
m1, b = parse("end")
daemon_pid = int(open(os.path.join(out, "daemon.pid")).read().strip())
hz = os.sysconf("SC_CLK_TCK")
wall_s = (m1 - m0) / 1e9
tot = 0
print("PROC_WALL_S %.6f  CLK_TCK %d" % (wall_s, hz))
for pid in sorted(a):
    if a[pid] is None or b.get(pid) is None:
        print("PROC_PID %d GONE (not counted)" % pid); continue
    ticks = b[pid] - a[pid]
    ns = ticks * 1e9 / hz
    tag = "DAEMON" if pid == daemon_pid else "BACKEND"
    print("PROC_%s_PID %d TICKS %d CPU_NS %.0f" % (tag, pid, ticks, ns))
    if pid != daemon_pid:
        tot += ns
print("PROC_CLIENT_TOTAL_CPU_NS %.0f" % tot)
print("PROC_CLIENT_TOTAL_CPU_OVER_WALL %.4f" % (tot / 1e9 / wall_s))
try:
    m = json.load(open(os.path.join(out, "metrics.json")))
except Exception as e:
    print("DIAG-ERROR: metrics.json unparseable: %s" % e); sys.exit(1)
keys = ["noncmd_cpu_samples_total", "sampled_attr_tick_read_failures_total",
        "sample_read_faults_total", "cmd_gate_recovered_total",
        "invalid_wait_reads_total", "samples_written_total",
        "sampled_attr_shadow_mismatch_total", "sampled_attr_shadow_checks_total"]
for k in keys:
    print("METRIC %s %s" % (k, m.get(k, "ABSENT-from-metrics")))
PYEOF
