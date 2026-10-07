#!/bin/bash
# diag294.sh RATE OUTDIR REPO
#
# Fork of /root/diag_sampler_cpu_bias.sh (issue #294 diagnostic), with two
# changes only:
#   - XVAL_BIN lets BOTH trees be compared by the SAME comparator binary, so a
#     base/fix difference cannot come from the comparator.
#   - the metric key list includes the #294 recheck counters.
# Every step that cannot produce its measurement prints DIAG-ERROR and exits
# nonzero: a missing measurement must never read as a zero.
set -uo pipefail

RATE="${1:?usage: $0 RATE OUTDIR REPO}"
OUT="${2:?usage: $0 RATE OUTDIR REPO}"
REPO="${3:?usage: $0 RATE OUTDIR REPO}"
ESC_SECONDS="${ESC_SECONDS:-60}"
CLIENTS="${CLIENTS:-8}"
PGPORT_USE="${PGPORT_USE:-5418}"

TRACER="$REPO/pg_wait_tracer"
XVAL="${XVAL_BIN:-$REPO/tests/cross_validate}"

die() { echo "DIAG-ERROR: $*" >&2; exit 1; }

[[ -x "$TRACER" ]] || die "no daemon at $TRACER"
[[ -x "$XVAL" ]]   || die "no cross_validate at $XVAL"
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

echo "DIAG rate=${RATE}Hz tracer=$TRACER xval=$XVAL out=$OUT"

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

snap_cpu() {
    local tag="$1"; shift
    local f="$OUT/proc-$tag.txt"
    : > "$f"
    printf 'MONO_NS %s\n' "$(date +%s%N)" >> "$f"
    for pid in "$@"; do
        [[ -r "/proc/$pid/stat" ]] || { echo "GONE $pid" >> "$f"; continue; }
        awk -v p="$pid" '{
            n = index($0, ") ");
            rest = substr($0, n + 2);
            split(rest, a, " ");
            printf "PID %s UTIME %s STIME %s SUM %s\n", p, a[12], a[13], a[12]+a[13]
        }' "/proc/$pid/stat" >> "$f"
    done
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

pgbench -h /var/run/postgresql -p "$PGPORT_USE" -U postgres -d postgres \
    -c "$CLIENTS" -j 2 -T $((ESC_SECONDS + 10)) > "$OUT/pgbench.txt" 2>&1 &
PGB=$!
sleep 3

mapfile -t BACKENDS < <($PSQL "SELECT pid FROM pg_stat_activity WHERE backend_type='client backend' AND pid <> pg_backend_pid()")
[[ ${#BACKENDS[@]} -ge 1 ]] || die "no client backends visible: the workload is not running"
echo "DIAG client_backends=${#BACKENDS[@]}"

RESP=$(ctl "{\"cmd\":\"escalate\",\"duration_s\":${ESC_SECONDS},\"reason\":\"cpu-bias-294\"}")
echo "DIAG escalate: $RESP"
[[ "$RESP" == *'"ok"'* || "$RESP" == *'escalat'* ]] || die "escalate refused: $RESP"

snap_cpu start "${BACKENDS[@]}" "$TPID"
sleep $((ESC_SECONDS - 2))
snap_cpu end "${BACKENDS[@]}" "$TPID"

ctl '{"cmd":"metrics"}' > "$OUT/metrics.json" || die "metrics read failed"
[[ -s "$OUT/metrics.json" ]] || die "metrics.json is empty -- counters unmeasured, not zero"
echo

sleep 3
kill "$PGB" 2>/dev/null; wait "$PGB" 2>/dev/null
kill -TERM "$TPID" 2>/dev/null; wait "$TPID" 2>/dev/null

echo "DIAG pgbench: $(grep -E '^tps' "$OUT/pgbench.txt" | tr '\n' ' ')"

"$XVAL" "$TRACE_DIR" --tolerance 10 --raw-ns > "$OUT/xval.txt" 2>&1
echo "DIAG cross_validate exit=$?"
grep -E "^(CPU|Max share|RESULT)|^RAW_" "$OUT/xval.txt"

python3 - "$OUT" <<'PYEOF'
import json, os, sys
out = sys.argv[1]
def parse(tag):
    d = {}; mono = None
    for line in open(os.path.join(out, "proc-%s.txt" % tag)):
        f = line.split()
        if f[0] == "MONO_NS": mono = int(f[1]); continue
        if f[0] == "GONE":    d[int(f[1])] = None; continue
        d[int(f[1])] = int(f[7])
    return mono, d
m0, a = parse("start"); m1, b = parse("end")
daemon_pid = int(open(os.path.join(out, "daemon.pid")).read().strip())
hz = os.sysconf("SC_CLK_TCK"); wall_s = (m1 - m0) / 1e9
tot = 0
print("PROC_WALL_S %.6f  CLK_TCK %d" % (wall_s, hz))
for pid in sorted(a):
    if a[pid] is None or b.get(pid) is None:
        print("PROC_PID %d GONE (not counted)" % pid); continue
    ticks = b[pid] - a[pid]; ns = ticks * 1e9 / hz
    tag = "DAEMON" if pid == daemon_pid else "BACKEND"
    print("PROC_%s_PID %d TICKS %d CPU_NS %.0f" % (tag, pid, ticks, ns))
    if pid != daemon_pid: tot += ns
print("PROC_CLIENT_TOTAL_CPU_NS %.0f" % tot)
try:
    m = json.load(open(os.path.join(out, "metrics.json")))
except Exception as e:
    print("DIAG-ERROR: metrics.json unparseable: %s" % e); sys.exit(1)
for k in ["noncmd_cpu_samples_total", "cmd_gate_order_at_risk_total",
          "cmd_gate_order_recovered_total", "cmd_gate_order_read_failed_total",
          "cmd_gate_recovered_total", "samples_total",
          "sampled_attr_tick_read_failures_total",
          "sampled_attr_shadow_cmd_open_mismatch_total",
          "sampled_attr_shadow_total", "invalid_wait_reads_total",
          "sampled_idle_in_command_total"]:
    print("METRIC %s %s" % (k, m.get(k, "ABSENT-from-metrics")))
PYEOF
