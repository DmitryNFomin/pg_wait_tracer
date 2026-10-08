#!/bin/bash
# arm294.sh RATE OUTDIR REPO   (issue #294 residual probe, arms A/B/C)
#
# Fork of tests/results/sampler-294/diag294.sh with exactly four additions, all
# of them measurement:
#   - RT_PRIO: when set, the daemon launches under `chrt -f $RT_PRIO`, and the
#     scheduling policy is then VERIFIED from /proc/<pid>/stat. A missing or
#     non-executable chrt (exit 126/127) would otherwise run arm B at normal
#     priority and read as "hypothesis disproved" -- the most dangerous false
#     negative in this experiment -- so it is a hard DIAG-ERROR. The baseline
#     arms assert policy==0 for the same reason in the other direction.
#   - ARM: a label echoed into the output for provenance.
#   - cross_validate is called with --show-idle, so the non-DB-Time destination
#     of the missing CPU is PRINTED rather than inferred; exit 4 (the
#     comparator refusing because the pacing mask is empty) aborts the run.
#   - the client-backend count actually observed is asserted against CLIENTS,
#     so arm C cannot silently run arm A's workload.
#
# Every step that cannot produce its measurement prints DIAG-ERROR and exits
# nonzero: a missing measurement must never read as a zero.
set -uo pipefail

RATE="${1:?usage: $0 RATE OUTDIR REPO}"
OUT="${2:?usage: $0 RATE OUTDIR REPO}"
REPO="${3:?usage: $0 RATE OUTDIR REPO}"
ESC_SECONDS="${ESC_SECONDS:-60}"
CLIENTS="${CLIENTS:-8}"
PGPORT_USE="${PGPORT_USE:-5418}"
RT_PRIO="${RT_PRIO:-}"
ARM="${ARM:-?}"

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

echo "DIAG arm=$ARM rate=${RATE}Hz clients=$CLIENTS rt_prio=${RT_PRIO:-none}"
echo "DIAG tracer=$TRACER xval=$XVAL out=$OUT"

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

if [[ -n "$RT_PRIO" ]]; then
    command -v chrt >/dev/null || die "chrt not on PATH (exit 127 case): arm B cannot run"
    chrt -f "$RT_PRIO" /bin/true || die "chrt -f $RT_PRIO exited $?: cannot set SCHED_FIFO"
    chrt -f "$RT_PRIO" "$TRACER" --daemon --pid "$PM_PID" -i 1 -T "$TRACE_DIR" \
        --mode tiered --sample-rate "$RATE" --escalation-budget 600 -q \
        >/dev/null 2>"$LOG" &
else
    "$TRACER" --daemon --pid "$PM_PID" -i 1 -T "$TRACE_DIR" \
        --mode tiered --sample-rate "$RATE" --escalation-budget 600 -q \
        >/dev/null 2>"$LOG" &
fi
TPID=$!
echo "$TPID" > "$OUT/daemon.pid"
# Any die() past this point must not leave a daemon (and later a pgbench)
# running on a shared box: the first smoke run of this script orphaned both.
cleanup() { [[ -n "${TPID:-}" ]] && kill -TERM "$TPID" 2>/dev/null; [[ -n "${PGB:-}" ]] && kill "$PGB" 2>/dev/null; }
trap cleanup EXIT
for _ in $(seq 1 40); do [[ -S "$SOCK" ]] && break; sleep 0.3; done
[[ -S "$SOCK" ]] || { tail -20 "$LOG"; kill "$TPID" 2>/dev/null; die "daemon never created $SOCK"; }

# VERIFY the scheduling policy actually took. /proc/<pid>/stat after "comm) ":
# field 40 = rt_priority -> a[38], field 41 = policy -> a[39]
# (SCHED_OTHER=0, SCHED_FIFO=1). An arm that silently ran at the wrong policy
# must FAIL, never be reported as a null result.
POLICY=$(awk '{n=index($0,") "); split(substr($0,n+2),a," "); print a[39]}' "/proc/$TPID/stat")
RTPRI=$(awk '{n=index($0,") "); split(substr($0,n+2),a," "); print a[38]}' "/proc/$TPID/stat")
echo "DIAG daemon_sched policy=$POLICY rt_priority=$RTPRI" | tee "$OUT/sched.txt"
[[ -n "$POLICY" ]] || { kill "$TPID" 2>/dev/null; die "could not read the daemon's scheduling policy"; }
if [[ -n "$RT_PRIO" ]]; then
    [[ "$POLICY" == "1" ]] || { kill "$TPID" 2>/dev/null; die "RT arm but policy=$POLICY (want 1=SCHED_FIFO): the arm did NOT run real-time"; }
    [[ "$RTPRI" == "$RT_PRIO" ]] || { kill "$TPID" 2>/dev/null; die "RT arm but rt_priority=$RTPRI (want $RT_PRIO)"; }
else
    [[ "$POLICY" == "0" ]] || { kill "$TPID" 2>/dev/null; die "baseline arm but policy=$POLICY (want 0=SCHED_OTHER)"; }
fi

ctl '{"cmd":"status"}'  > "$OUT/status-before.json" || die "status failed"
echo

pgbench -h /var/run/postgresql -p "$PGPORT_USE" -U postgres -d postgres \
    -c "$CLIENTS" -j 2 -T $((ESC_SECONDS + 10)) > "$OUT/pgbench.txt" 2>&1 &
PGB=$!
sleep 3

mapfile -t BACKENDS < <($PSQL "SELECT pid FROM pg_stat_activity WHERE backend_type='client backend' AND pid <> pg_backend_pid()")
[[ ${#BACKENDS[@]} -ge 1 ]] || die "no client backends visible: the workload is not running"
echo "DIAG client_backends=${#BACKENDS[@]}"
# Arm C's whole claim is "fewer runnable backends than cores". If pgbench
# actually opened arm A's 8 connections, arm C measured arm A.
[[ ${#BACKENDS[@]} -eq $CLIENTS ]] || die "expected $CLIENTS client backends, saw ${#BACKENDS[@]}: this arm did not run its own workload"

RESP=$(ctl "{\"cmd\":\"escalate\",\"duration_s\":${ESC_SECONDS},\"reason\":\"cpu-bias-294-arm-$ARM\"}")
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

"$XVAL" "$TRACE_DIR" --tolerance 10 --raw-ns --show-idle > "$OUT/xval.txt" 2>&1
XRC=$?
echo "DIAG cross_validate exit=$XRC"
[[ "$XRC" == 4 ]] && die "cross_validate REFUSED the idle breakdown (exit 4): pacing mask empty, so the non-DB-Time destination is unmeasured, not zero"
grep -E "^(CPU|Max share|RESULT)|^RAW_|^IDLE" "$OUT/xval.txt"

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
          "sampled_idle_in_command_total", "ticks_missed_total"]:
    print("METRIC %s %s" % (k, m.get(k, "ABSENT-from-metrics")))
PYEOF
