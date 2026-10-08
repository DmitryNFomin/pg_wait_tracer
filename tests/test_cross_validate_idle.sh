#!/usr/bin/env bash
# test_cross_validate_idle.sh — cross_validate --show-idle: the destination of
# the time that is NOT DB Time.
#
# WHY (issue #294): the comparator drops pgwt_is_idle_event() time from BOTH
# sides before computing any share, so when the sampled tier reads less CPU
# than the exact tier the missing nanoseconds have no visible destination —
# "it reappeared as ClientRead/idle" could only be INFERRED from an AAS
# delta. --show-idle prints it.
#
# WHAT IS GATED, and what is deliberately not:
#   - the idle totals and per-event rows are exact, from a hand-built trace
#     whose expected nanoseconds are written out as literal constants below
#     (derived by hand in the comment block, NOT recomputed by this script
#     from the same traversal the tool uses — otherwise both sides of the
#     comparison would come from one source and the check could not fail);
#   - --show-idle must not move a single byte of the pre-existing output,
#     including RESULT and the exit code;
#   - and the BYPASS SUITE below: every way this breakdown can be made
#     unreachable or satisfiable-without-measuring must refuse or go red.
#     The dangerous direction here is a FALSE NEGATIVE — an idle section that
#     prints 0 ns because nothing could be classified reads exactly like
#     "no time left DB Time", which is the opposite conclusion.
#
# Needs only tests/gen_test_traces and tests/cross_validate: no PostgreSQL, no
# BPF, no root. Overridable for the red-vs-green demonstration:
#   XVAL=/path/to/cross_validate GEN=/path/to/gen_test_traces ./test_cross_validate_idle.sh
set -uo pipefail

cd "$(dirname "$0")" || exit 1
XVAL="${XVAL:-./cross_validate}"
GEN="${GEN:-./gen_test_traces}"

pass=0; fail=0
ok()   { pass=$((pass+1)); echo "  ok: $1"; }
bad()  { fail=$((fail+1)); echo "  FAIL: $1"; }
chk()  { if [[ "$2" == "$3" ]]; then ok "$1 = $2"; else bad "$1: got '$2' want '$3'"; fi; }

TMP=$(mktemp -d /tmp/pgwt_xvidle_XXXXXX) || exit 1
trap 'rm -rf "$TMP"' EXIT

# A tool that is not there must FAIL this test, never skip it: exit 126/127 on
# a dependency is how a gate silently stops gating.
for b in "$XVAL" "$GEN"; do
    if [[ ! -x "$b" ]]; then
        echo "FAIL: $b is missing or not executable — run 'make -C tests' first."
        echo "      (A missing dependency fails this test; it never skips it.)"
        exit 1
    fi
done
# --pg-version is the ONLY way to reach the empty-pacing-mask path (bypass B1),
# so probe the BINARY's behaviour -- not its --help text, and not the source
# file sitting next to it. A stale $GEN beside a fresh gen_test_traces.c would
# otherwise be waved through by a source grep while B1 went unexercised.
probe="$(mktemp -d "$TMP/probe.XXXX")"
cat > "$TMP/probe.json" <<'JSON'
{"backends":[{"pid":1,"type":"client","user":"postgres","db":"postgres"}],
 "sample_period_ns":1000000000,
 "events":[{"pid":1,"ts":1000000000,"dur":1000000,"old":0,"new":0}],
 "samples":[{"pid":1,"ts":1000000000,"event":0}]}
JSON
if ! "$GEN" -o "$probe" -s "$TMP/probe.json" --pg-version 16 >/dev/null 2>&1; then
    echo "FAIL: $GEN does not accept --pg-version, so bypass case B1 (the"
    echo "      empty pacing mask) cannot be reached and its refusal would go"
    echo "      untested. Rebuild tests/gen_test_traces from this tree."
    exit 1
fi

# ── The fixture ──────────────────────────────────────────────────────────
# Event ids (src/pg_wait_tracer.h: class<<24 | id):
#   CPU                              0
#   Client:ClientRead                0x06000000 = 100663296   IDLE (by class)
#   Timeout:CheckpointWriteDelay     0x09000001 = 150994945   IDLE (pacing mask)
#   Timeout:SpinDelay                0x09000006 = 150994950   DB Time (not pacing)
#   Lock:relation                    0x03000001 =  50331649   DB Time
#
# Samples: 1 Hz (period 1e9), ts 10e9..30e9 inclusive = 21 samples.
# Transitions: ts 12e9..28e9. So tr_first=12e9, tr_last=28e9, sm_first=10e9,
# sm_last=30e9  =>  overlap window = [12e9, 28e9), 16.0 s.
#
# EXPECTED, worked out by hand:
#  exact side (old_event, dur, clipped to the window):
#    12e9 CPU  1e9 -> interval [11e9,12e9] clips to zero  (boundary throwaway)
#    14e9 CPU  2e9 -> CPU   2e9
#    16e9 CR   2e9 -> IDLE  2e9
#    18e9 CPU  2e9 -> CPU   2e9
#    20e9 CWD  1e9 -> IDLE  1e9
#    22e9 LOCK 2e9 -> Lock  2e9
#    24e9 SPIN 1e9 -> SpinDelay 1e9   (Timeout but NOT pacing => DB Time)
#    26e9 CPU  2e9 -> CPU   2e9
#    28e9 CR   1e9 -> IDLE  1e9       (ends exactly at win_to, fully counted)
#  => exact DB Time = 2+2+2 CPU + 2 Lock + 1 Spin        =  9e9
#  => exact IDLE    = 2+1 ClientRead + 1 CheckpointWriteDelay = 4e9
#  => accepted interval time altogether                  = 13e9
#
#  sampled side: a sample counts iff win_from <= ts < win_to, i.e. ts in
#  [12e9, 27e9]: 16 samples.
#    8 x CPU (12..19e9), 3 x CR (20..22e9), 1 x CWD (23e9),
#    2 x LOCK (24,25e9), 2 x SPIN (26,27e9)
#  The 5 ClientRead samples at 10,11,28,29,30 e9 are OUTSIDE the window: if
#  the idle table were not window-clipped they would show up as 9e9, not 4e9.
#  => sampled DB Time = 8e9 CPU + 2e9 Lock + 2e9 Spin = 12e9
#  => sampled IDLE    = 3e9 CR + 1e9 CWD              =  4e9, from 4 samples
#  => IDLE_EXACT_PCT_OF_DBTIME   = 100*4/9  = 44.44
#     IDLE_SAMPLED_PCT_OF_DBTIME = 100*4/12 = 33.33
EXP_TOTAL_EXACT=9000000000
EXP_TOTAL_SAMPLED=12000000000
EXP_CPU_EXACT=6000000000
EXP_CPU_SAMPLED=8000000000
EXP_IDLE_EXACT=4000000000
EXP_IDLE_SAMPLED=4000000000
EXP_IDLE_SAMPLES=4
EXP_ACCEPTED_TOTAL=13000000000     # exact DB Time + exact idle, stated, not summed by the tool

CR=100663296
CWD=150994945
SPIN=150994950
LOCK=50331649

write_scenario() {   # write_scenario <file> <idle-mode>
    local f="$1" mode="$2"
    local cr=$CR cwd=$CWD
    if [[ "$mode" == "no-idle" ]]; then
        # B2: a trace with NO idle event at all. ClientRead/pacing slots become
        # Lock, so the shares stay comparable but nothing is idle.
        cr=$LOCK; cwd=$LOCK
    fi
    local sm_cr=$cr
    if [[ "$mode" == "exact-only-idle" ]]; then
        # B6: idle present on the EXACT tier only. Every sampled idle slot
        # becomes CPU, so a one-sided idle loss must still print a row.
        sm_cr=0; cwd=$CWD
    fi
    cat > "$f" <<JSON
{
  "backends": [{"pid": 1000, "type": "client", "user": "postgres", "db": "postgres"}],
  "sample_period_ns": 1000000000,
  "events": [
    {"pid": 1000, "ts": 12000000000, "dur": 1000000000, "old": 0,     "new": $cr},
    {"pid": 1000, "ts": 14000000000, "dur": 2000000000, "old": 0,     "new": $cr},
    {"pid": 1000, "ts": 16000000000, "dur": 2000000000, "old": $cr,   "new": 0},
    {"pid": 1000, "ts": 18000000000, "dur": 2000000000, "old": 0,     "new": $cwd},
    {"pid": 1000, "ts": 20000000000, "dur": 1000000000, "old": $cwd,  "new": 0},
    {"pid": 1000, "ts": 22000000000, "dur": 2000000000, "old": $LOCK, "new": 0},
    {"pid": 1000, "ts": 24000000000, "dur": 1000000000, "old": $SPIN, "new": 0},
    {"pid": 1000, "ts": 26000000000, "dur": 2000000000, "old": 0,     "new": $cr},
    {"pid": 1000, "ts": 28000000000, "dur": 1000000000, "old": $cr,   "new": 0}
  ],
  "samples": [
    {"pid": 1000, "ts": 10000000000, "event": $sm_cr},
    {"pid": 1000, "ts": 11000000000, "event": $sm_cr},
    {"pid": 1000, "ts": 12000000000, "event": 0},
    {"pid": 1000, "ts": 13000000000, "event": 0},
    {"pid": 1000, "ts": 14000000000, "event": 0},
    {"pid": 1000, "ts": 15000000000, "event": 0},
    {"pid": 1000, "ts": 16000000000, "event": 0},
    {"pid": 1000, "ts": 17000000000, "event": 0},
    {"pid": 1000, "ts": 18000000000, "event": 0},
    {"pid": 1000, "ts": 19000000000, "event": 0},
    {"pid": 1000, "ts": 20000000000, "event": $sm_cr},
    {"pid": 1000, "ts": 21000000000, "event": $sm_cr},
    {"pid": 1000, "ts": 22000000000, "event": $sm_cr},
    {"pid": 1000, "ts": 23000000000, "event": $cwd},
    {"pid": 1000, "ts": 24000000000, "event": $LOCK},
    {"pid": 1000, "ts": 25000000000, "event": $LOCK},
    {"pid": 1000, "ts": 26000000000, "event": $SPIN},
    {"pid": 1000, "ts": 27000000000, "event": $SPIN},
    {"pid": 1000, "ts": 28000000000, "event": $sm_cr},
    {"pid": 1000, "ts": 29000000000, "event": $sm_cr},
    {"pid": 1000, "ts": 30000000000, "event": $sm_cr}
  ]
}
JSON
}

field() { grep -E "^$2 " "$1" | head -1 | awk '{print $2}'; }

# ── 1. true positive: the idle rows and their nanoseconds ────────────────
echo "== 1. idle breakdown on a hand-built trace =="
D="$TMP/t1"; mkdir -p "$D"
write_scenario "$TMP/s1.json" normal
"$GEN" -o "$D" -s "$TMP/s1.json" >/dev/null || { echo "FAIL: generator"; exit 1; }
"$XVAL" "$D" --raw-ns --show-idle > "$TMP/o1.txt" 2>&1
rc=$?
chk "exit code (mask resolved, shares within tolerance)" "$rc" "0"
chk "RAW_TOTAL_EXACT_NS"   "$(field "$TMP/o1.txt" RAW_TOTAL_EXACT_NS)"   "$EXP_TOTAL_EXACT"
chk "RAW_TOTAL_SAMPLED_NS" "$(field "$TMP/o1.txt" RAW_TOTAL_SAMPLED_NS)" "$EXP_TOTAL_SAMPLED"
chk "RAW_CPU_EXACT_NS"     "$(field "$TMP/o1.txt" RAW_CPU_EXACT_NS)"     "$EXP_CPU_EXACT"
chk "RAW_CPU_SAMPLED_NS"   "$(field "$TMP/o1.txt" RAW_CPU_SAMPLED_NS)"   "$EXP_CPU_SAMPLED"
chk "IDLE_EVENT_ROWS"        "$(field "$TMP/o1.txt" IDLE_EVENT_ROWS)"        "2"
chk "IDLE_TOTAL_EXACT_NS"    "$(field "$TMP/o1.txt" IDLE_TOTAL_EXACT_NS)"    "$EXP_IDLE_EXACT"
chk "IDLE_TOTAL_SAMPLED_NS"  "$(field "$TMP/o1.txt" IDLE_TOTAL_SAMPLED_NS)"  "$EXP_IDLE_SAMPLED"
chk "IDLE_SAMPLES"           "$(field "$TMP/o1.txt" IDLE_SAMPLES)"           "$EXP_IDLE_SAMPLES"
chk "IDLE_EXACT_PCT_OF_DBTIME"   "$(field "$TMP/o1.txt" IDLE_EXACT_PCT_OF_DBTIME)"   "44.44"
chk "IDLE_SAMPLED_PCT_OF_DBTIME" "$(field "$TMP/o1.txt" IDLE_SAMPLED_PCT_OF_DBTIME)" "33.33"
# The whole point of the change: ClientRead has to be VISIBLE and labelled.
if grep -q "ClientRead" "$TMP/o1.txt"; then ok "ClientRead appears in the output"
else bad "ClientRead is still invisible — the change does nothing"; fi
if grep -qi "NOT DB Time" "$TMP/o1.txt"; then ok "the section is labelled non-DB-Time"
else bad "the idle section is not labelled as non-DB-Time"; fi
# Per-row nanoseconds, so a total that is right by cancellation cannot pass.
# Scoped to the IDLE SECTION, not the whole file: the share table above uses
# the same left-aligned name column, so an unscoped grep would read a DB-Time
# row as an idle row (that is how the SpinDelay check below first went red
# against a tool that was behaving correctly).
sect() { awk '/^=== IDLE: NOT DB Time/{s=1} /^RAW_WINDOW_S /{s=0} s' "$1"; }
sect "$TMP/o1.txt" > "$TMP/o1-idle.txt"
cr_row=$(grep -E "^ClientRead " "$TMP/o1-idle.txt" | head -1)
# Guard against the anchors themselves going stale: if pgwt_event_name's
# spelling changes, these row assertions must FAIL, not pass on empty strings.
[[ -n "$cr_row" ]] || bad "no ClientRead ROW matched — the row anchors are stale, so every per-row assertion below would pass vacuously"
chk "ClientRead exact_ns"   "$(echo "$cr_row" | awk '{print $2}')" "3000000000"
chk "ClientRead sampled_ns" "$(echo "$cr_row" | awk '{print $3}')" "3000000000"
cwd_row=$(grep -E "^CheckpointWriteDelay " "$TMP/o1-idle.txt" | head -1)
chk "CheckpointWriteDelay exact_ns"   "$(echo "$cwd_row" | awk '{print $2}')" "1000000000"
chk "CheckpointWriteDelay sampled_ns" "$(echo "$cwd_row" | awk '{print $3}')" "1000000000"
# Timeout:SpinDelay is a pg_usleep backoff inside an OUTSTANDING acquisition:
# DB Time, so it must NOT be swept into the idle table.
if grep -E "^SpinDelay " "$TMP/o1-idle.txt" | head -1 | grep -q .; then
    bad "SpinDelay is in the IDLE table — the pacing set has widened"
else ok "SpinDelay stays in DB Time (not swept into idle)"; fi
# Conservation, with the two sides from DIFFERENT places: the tool's DB-Time
# total plus the tool's idle total must equal the fixture's hand-stated total
# accepted interval time. Both being wrong by the same amount cannot hide here
# because EXP_ACCEPTED_TOTAL is a literal, not a re-traversal.
# An ABSENT field must be reported as a failure, not crash the suite: when
# this ran against the pre-change comparator, $(( "" + "" )) aborted the script
# at this line and the entire bypass suite below never executed. A gate that
# dies on the way to its own cases cannot prove anything about them.
ex_ns=$(field "$TMP/o1.txt" RAW_TOTAL_EXACT_NS)
idle_ns=$(field "$TMP/o1.txt" IDLE_TOTAL_EXACT_NS)
if [[ -z "$ex_ns" || -z "$idle_ns" ]]; then
    bad "conservation unmeasurable: RAW_TOTAL_EXACT_NS='$ex_ns' IDLE_TOTAL_EXACT_NS='$idle_ns'"
else
    chk "exact DB Time + exact idle" "$((ex_ns + idle_ns))" "$EXP_ACCEPTED_TOTAL"
fi

# ── 2. purity: --show-idle changes nothing that already existed ──────────
echo "== 2. --show-idle does not perturb the share table, RESULT or exit code =="
"$XVAL" "$D" --raw-ns > "$TMP/o2-plain.txt" 2>&1; rc_plain=$?
chk "exit code without --show-idle" "$rc_plain" "0"
# Strip exactly the new section, then the rest must be byte-identical.
awk '/^=== IDLE: NOT DB Time/{s=1} /^RAW_WINDOW_S /{s=0} !s' "$TMP/o1.txt" \
    | grep -v '^IDLE' > "$TMP/o1-stripped.txt"
if diff -u "$TMP/o2-plain.txt" "$TMP/o1-stripped.txt" > "$TMP/purity.diff" 2>&1; then
    ok "pre-existing output is byte-identical with and without --show-idle"
else
    bad "--show-idle perturbed existing output:"; sed 's/^/      /' "$TMP/purity.diff"
fi

# ── BYPASS SUITE: every way the breakdown can fail to detect ─────────────
# B1 — the pacing mask cannot be resolved. Client/Activity are classified by
# class, but the Timeout pacing subset comes from the resolved PG major, so an
# unverified major silently leaves CheckpointWriteDelay in DB Time and the idle
# totals undercount by an unknown amount while LOOKING measured.
echo "== B1. unresolvable pacing mask must REFUSE, not print zeros =="
D1="$TMP/b1"; mkdir -p "$D1"
"$GEN" -o "$D1" -s "$TMP/s1.json" --pg-version 16 >/dev/null || { echo "FAIL: generator --pg-version"; exit 1; }
"$XVAL" "$D1" --raw-ns --show-idle > "$TMP/b1.txt" 2>&1; rc1=$?
chk "exit code (cannot classify)" "$rc1" "4"
if grep -q "IDLE-UNAVAILABLE" "$TMP/b1.txt"; then ok "prints IDLE-UNAVAILABLE"
else bad "no IDLE-UNAVAILABLE line"; fi
if grep -q "^IDLE_TOTAL_EXACT_NS" "$TMP/b1.txt"; then
    bad "printed idle totals it could not measure (reads as a measured zero)"
else ok "printed no idle totals it could not measure"; fi
chk "IDLE_PACING_MASK" "$(field "$TMP/b1.txt" IDLE_PACING_MASK)" "0x0"
# and the refusal must not corrupt the share verdict that was asked for too
if grep -q "^RESULT: " "$TMP/b1.txt"; then ok "the share RESULT is still printed"
else bad "the refusal swallowed the share RESULT"; fi

# B2 — the thing being checked is ABSENT rather than wrong: a trace with no
# idle event at all must say so, in words, with the mask it resolved. "No idle
# events in this trace" and "nothing could be classified" must never look alike.
echo "== B2. genuine absence of idle events is stated, not implied =="
D2="$TMP/b2"; mkdir -p "$D2"
write_scenario "$TMP/s2.json" no-idle
"$GEN" -o "$D2" -s "$TMP/s2.json" >/dev/null || { echo "FAIL: generator"; exit 1; }
"$XVAL" "$D2" --show-idle > "$TMP/b2.txt" 2>&1; rc2=$?
chk "exit code (mask fine, no idle data)" "$rc2" "0"
if grep -q "^IDLE_EVENTS none" "$TMP/b2.txt"; then ok "absence stated explicitly"
else bad "absence is silent — an empty table reads like a measured zero"; fi
chk "IDLE_TOTAL_EXACT_NS"   "$(field "$TMP/b2.txt" IDLE_TOTAL_EXACT_NS)"   "0"
chk "IDLE_TOTAL_SAMPLED_NS" "$(field "$TMP/b2.txt" IDLE_TOTAL_SAMPLED_NS)" "0"
if grep -q "IDLE-UNAVAILABLE" "$TMP/b2.txt"; then
    bad "refused a trace it could classify perfectly well"
else ok "did not refuse a classifiable trace"; fi

# B3 — empty input. No trace files at all must ERROR, never report a 0 ns idle
# total from nothing.
echo "== B3. empty trace dir must error, not report zero idle =="
D3="$TMP/b3"; mkdir -p "$D3"
"$XVAL" "$D3" --show-idle > "$TMP/b3.txt" 2>&1; rc3=$?
if [[ "$rc3" != 0 ]]; then ok "nonzero exit ($rc3) on an empty dir"
else bad "exit 0 on an empty trace dir"; fi
if grep -q "^IDLE_TOTAL" "$TMP/b3.txt"; then
    bad "reported idle totals for a directory with no trace in it"
else ok "no idle totals from an empty dir"; fi

# B4 — a PARTIAL capture: sampled blocks only, no exact tier. The existing tool
# errors; the idle section must not sneak a half-answer out ahead of it.
echo "== B4. sampled-only trace (no exact tier) must error, not half-answer =="
D4="$TMP/b4"; mkdir -p "$D4"
python3 - "$TMP/s1.json" "$TMP/s4.json" <<'PY'
import json, re, sys
raw = open(sys.argv[1]).read()
# drop the "events" array -> SAMPLES blocks only
out = re.sub(r'"events"\s*:\s*\[.*?\],\s*', '', raw, flags=re.S)
json.loads(out)   # must still be valid JSON, or the case is not what it claims
open(sys.argv[2], "w").write(out)
PY
"$GEN" -o "$D4" -s "$TMP/s4.json" >/dev/null || { echo "FAIL: generator"; exit 1; }
"$XVAL" "$D4" --show-idle > "$TMP/b4.txt" 2>&1; rc4=$?
if [[ "$rc4" != 0 ]]; then ok "nonzero exit ($rc4) with no exact tier"
else bad "exit 0 on a trace with no exact tier"; fi
if grep -q "^IDLE_TOTAL" "$TMP/b4.txt"; then
    bad "printed idle totals with no exact tier to compare against"
else ok "no idle totals without an exact tier"; fi

# B5 — one-sided idle: present on the exact tier, zero on the sampled tier.
# This is the #294 shape exactly, so the row must survive with a 0 on one side
# rather than being dropped for having no sampled time.
echo "== B5. idle on the exact tier only must still print a row =="
D5="$TMP/b5"; mkdir -p "$D5"
write_scenario "$TMP/s5.json" exact-only-idle
"$GEN" -o "$D5" -s "$TMP/s5.json" >/dev/null || { echo "FAIL: generator"; exit 1; }
"$XVAL" "$D5" --show-idle > "$TMP/b5.txt" 2>&1
sect "$TMP/b5.txt" > "$TMP/b5-idle.txt"
cr5=$(grep -E "^ClientRead " "$TMP/b5-idle.txt" | head -1)
if [[ -n "$cr5" ]]; then ok "ClientRead row present despite 0 sampled ns"
else bad "the row vanished when one tier had none — a one-sided loss is invisible"; fi
chk "one-sided ClientRead exact_ns"   "$(echo "$cr5" | awk '{print $2}')" "3000000000"
chk "one-sided ClientRead sampled_ns" "$(echo "$cr5" | awk '{print $3}')" "0"

# B6 — the flag must be opt-in: without it, not one byte of idle output.
echo "== B6. no --show-idle, no idle output =="
if "$XVAL" "$D" --raw-ns 2>&1 | grep -q "IDLE"; then
    bad "idle output leaked into a run that did not ask for it"
else ok "flag is opt-in"; fi

echo ""
echo "test_cross_validate_idle: $pass passed, $fail failed"
[[ "$fail" -eq 0 ]]
