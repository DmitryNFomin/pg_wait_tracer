#!/usr/bin/env bash
# fixture-discrimination.sh — does tests/test_v9_open_cpu_wired.sh tell
# "the writer was never ASKED" from "it was asked and credited nothing"?
#
# WHY. On the gate capture of 2026-10-28 (tests/results/box-check-ubuntu-
# 20261028-121033.log, commit 31a03da6, banner line 10820) that fixture
# printed:
#
#     ok: watchpoints attached (mask=15, tier=escalated)
#     ok: all seven v9 control keys present and numeric
#     ok: 3 of 3 CPU statements completed, spanning 18s of ticks
#     ok: stimulus PROVEN -- cpu_ns_total moved 795480753 -> 58758673391
#     summary_open_cpu_credited_ns   = 0
#     summary_open_cpu_refused_total = 0
#     summary_cpu_open_uncredited_ns_total = 0
#     summary_open_credited_ns_total = 75854656030  (the discriminator)
#     FAIL: ... the producer is not wired
#
# Every precondition held. But "credited 0 / refused 0 / uncredited 0" is
# TWO states — never asked, and asked-then-dropped — and the fixture had no
# reading that separated them, so "the producer is not wired" was a guess
# between two defects in two different files. summary_open_cpu_notes_total
# and the fixture's fifth precondition exist to end that guess.
#
# THIS SCRIPT IS THE NEGATIVE CONTROL for that precondition. It drives the
# real fixture against a FAKE daemon whose control socket answers exactly
# the JSON each scenario needs, so the decision logic is exercised with no
# PostgreSQL, no BPF and no box. Four scenarios, and the one that matters is
# C: the PARENT fixture calls it FAIL ("the producer is not wired") where the
# new one REFUSES ("the writer was never asked").
#
# Usage: tests/results/issue424-notes-counter/fixture-discrimination.sh
#        [path-to-fixture]        (default: the branch's own)
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
FIXTURE="${1:-$REPO/tests/test_v9_open_cpu_wired.sh}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/v9fakeXXXXXX")"
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/tests" "$WORK/bin"
cp "$FIXTURE" "$WORK/tests/test_v9_open_cpu_wired.sh"
cp "$REPO/tests/testutil.sh" "$WORK/tests/testutil.sh"
chmod +x "$WORK/tests/test_v9_open_cpu_wired.sh"

# ── the fake daemon ──────────────────────────────────────────────────────
# A unix-socket server answering `status` and `metrics`. cpu_ns_total rises
# on every metrics read, so the fixture's "stimulus PROVEN" check passes on
# merit rather than by being bypassed. SCEN selects what the v9 counters say.
cat > "$WORK/bin/fakeyd.py" <<'PYEOF'
import json, os, socket, sys

sock_path, scen = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.bind(sock_path)
s.listen(16)

STATUS = {"ok": True, "mode": "full", "tier": "escalated",
          "exact_probe_attached_mask": 15, "uptime_s": 12.0}

# notes / credited / present-ness of the notes key, per scenario.
SCEN = {
    # A: wired and crediting -> PASS
    "A": dict(notes=4210, credited=9123456789, v8=75854656030, key=True),
    # B: nothing sealed at all -> REFUSE (the pre-existing fourth outcome)
    "B": dict(notes=0, credited=0, v8=0, key=True),
    # C: THE 2026-10-28 CAPTURE. Seconds sealed, the writer never asked.
    "C": dict(notes=0, credited=0, v8=75854656030, key=True),
    # D: asked 4210 times and credited nothing -> FAIL, and only here.
    "D": dict(notes=4210, credited=0, v8=75854656030, key=True),
    # E: a daemon that predates the counter -> REFUSE, never a verdict.
    "E": dict(notes=0, credited=0, v8=75854656030, key=False),
}[scen]

calls = 0
def metrics():
    global calls
    calls += 1
    m = {"ok": True, "uptime_s": 12.0 + calls,
         "events_total": 1231776, "cpu_ns_total": 795480753 * calls,
         "summary_open_cpu_credited_ns": SCEN["credited"],
         "summary_open_cpu_measured_ns": 0,
         "summary_open_cpu_unknown_ns": 0,
         "summary_open_cpu_eff_rejected_total": 0,
         "summary_open_cpu_class_divergent_ns": 0,
         "summary_open_cpu_tier_mismatch_total": 0,
         "summary_open_cpu_overcredit_ns": 0,
         "summary_open_cpu_refused_total": 0,
         "summary_cpu_open_uncredited_ns_total": 0,
         "summary_open_credited_ns_total": SCEN["v8"],
         "summary_open_io_worker_refused_total": 0,
         "summary_open_table_full_total": 0,
         "summary_open_notes_expired_total": 0,
         "tier": "escalated"}
    if SCEN["key"]:
        m["summary_open_cpu_notes_total"] = SCEN["notes"]
    return m

while True:
    try:
        c, _ = s.accept()
    except OSError:
        break
    buf = b""
    while b"\n" not in buf:
        chunk = c.recv(4096)
        if not chunk:
            break
        buf += chunk
    try:
        req = json.loads(buf.decode().split("\n")[0])
    except Exception:
        req = {}
    out = STATUS if req.get("cmd") == "status" else metrics()
    c.sendall((json.dumps(out) + "\n").encode())
    c.close()
PYEOF

# The fixture launches "$SCRIPT_DIR/../pg_wait_tracer"; this stands in for it
# and parses -T the same way, so the socket lands where the fixture looks.
cat > "$WORK/pg_wait_tracer" <<'TREOF'
#!/usr/bin/env bash
dir=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        -T) dir="$2"; shift 2 ;;
        *) shift ;;
    esac
done
exec python3 "$(dirname "$0")/bin/fakeyd.py" "$dir/pgwt.sock" "${SCEN:?}"
TREOF
chmod +x "$WORK/pg_wait_tracer"

# A CPU statement that takes 4 s, so the fixture's ">= 3 s of ticks" and
# "3 of 3 completed" preconditions are met for real. `timeout` is stubbed
# because macOS has none; it must still pass the exit status through.
printf '#!/bin/sh\nsleep 4\nexit 0\n' > "$WORK/bin/psql"
printf '#!/bin/sh\nshift\nexec "$@"\n' > "$WORK/bin/timeout"
chmod +x "$WORK/bin/psql" "$WORK/bin/timeout"

echo "fixture under test: $FIXTURE"
echo ""
rc_of() {
    local scen="$1"
    SCEN="$scen" PATH="$WORK/bin:$PATH" \
        "$WORK/tests/test_v9_open_cpu_wired.sh" --pid 1 \
        > "$WORK/out.$scen" 2>&1
    echo "$?"
}

declare -A WANT=( [A]="PASS 0" [B]="REFUSE 2" [C]="REFUSE 2" \
                  [D]="FAIL 1" [E]="REFUSE 2" )
declare -A WHY=(
  [A]="asked 4210x and crediting -> the producer IS wired"
  [B]="nothing sealed (v8 credit 0 too) -> says nothing either way"
  [C]="THE 2026-10-28 CAPTURE: sealed, but the writer was NEVER ASKED"
  [D]="asked 4210x and credited nothing -> the CREDIT PATH dropped it"
  [E]="notes key absent (older daemon) -> cannot localise, must refuse"
)
fails=0
for scen in A B C D E; do
    rc=$(rc_of "$scen")
    verdict=$(/usr/bin/grep -oE 'test_v9_open_cpu_wired: (PASS|FAIL|REFUSE)' \
              "$WORK/out.$scen" | tail -1 | sed 's/.*: //')
    [[ -n "$verdict" ]] || verdict="(no verdict banner)"
    want_v=${WANT[$scen]% *}; want_rc=${WANT[$scen]#* }
    if [[ "$verdict" == "$want_v" && "$rc" == "$want_rc" ]]; then
        printf '  ok   %s: %-7s exit=%s   %s\n' "$scen" "$verdict" "$rc" "${WHY[$scen]}"
    else
        printf '  FAIL %s: got %s exit=%s, want %s exit=%s   %s\n' \
               "$scen" "$verdict" "$rc" "$want_v" "$want_rc" "${WHY[$scen]}"
        sed 's/^/        | /' "$WORK/out.$scen" | tail -12
        fails=$((fails + 1))
    fi
done
echo ""
echo "  --- scenario C, verbatim (the one the parent fixture gets wrong) ---"
sed 's/^/  | /' "$WORK/out.C"
echo ""
if [[ "$fails" -eq 0 ]]; then
    echo "=== fixture-discrimination: PASS (5 scenarios) ==="
    exit 0
fi
echo "=== fixture-discrimination: FAIL ($fails of 5) ==="
exit 1
