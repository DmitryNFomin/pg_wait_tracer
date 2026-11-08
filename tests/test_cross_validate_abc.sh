#!/usr/bin/env bash
# #115 step 1b: offline known-answer instrument test. No real-workload claims.
# Requires the C binaries and objects from make -C tests; never skip if absent.
set -euo pipefail
cd "$(dirname "$0")"
XVAL=${XVAL:-./cross_validate}
GEN=${GEN:-./gen_test_traces}
TMP=$(mktemp -d /tmp/pgwt_xvabc_XXXXXX)
trap 'rm -rf "$TMP"' EXIT
for b in "$XVAL" "$GEN"; do
    [[ -x "$b" ]] || { echo "FAIL: missing $b; run make -C tests cross_validate gen_test_traces"; exit 1; }
done
pass=0
chk() {
    if [[ "$2" != "$3" ]]; then
        echo "FAIL: $1: got '$2', expected '$3'"; exit 1
    fi
    pass=$((pass+1)); echo "  ok: $1"
}
field() { awk -v key="$2" '$1 == key {$1=""; sub(/^ +/, ""); print; exit}' "$1"; }
row() { awk -v key="$2" '$1 == "ABC_ROW" && $2 == key {$1=$2=""; sub(/^ +/, ""); print; exit}' "$1"; }
run() {
    local rc=0
    "$XVAL" "$1" --tolerance 100 --abc --nominal-period-ns 1000000000 "${@:4}" > "$2" 2>&1 || rc=$?
    chk "$(basename "$2") exit" "$rc" "$3"
}
generate() { "$GEN" -o "$1" -s "$2" > "$TMP/gen.log" 2>&1 || { cat "$TMP/gen.log"; exit 1; }; }

# The ten pre-existing row assertions (equal CPU/Lock/TOTAL, default TOTAL,
# weighted CPU/Lock/TOTAL, hole CPU/TOTAL, classification relation) retain
# their old shares/ns and add A_joined, the split deltas, and weighting.

# Literal hand calculation: overlap [10s,14s). The first transition ends at
# 10s and contributes zero. CPU covers [10,12), Lock [12,14). Four in-window
# samples at 10,11,12,13; 14 is outside. Each weighs 1s, giving 2s / 2s and
# 50/50 shares for A, B, and both Cs. The CPU interval carries KNOWN cpu_ns
# of only 0.4s: B/C must still classify its entire 2s as CPU wall time. Lock
# carries the UNKNOWN sentinel. Clipped TRANSITIONS counts = 1 known, 1
# unknown; joined samples = 2 known, 2 unknown. The transition instant 12
# belongs to Lock, never to the CPU interval that just ended.
cat > "$TMP/equal.json" <<'JSON'
{"backends":[{"pid":1,"type":"client","user":"postgres","db":"postgres"}],
 "cpu_measured":1,"sample_period_ns":1000000000,"interleave":1,
 "events":[
 {"pid":1,"ts":10000000000,"dur":1000000000,"old":0,"new":0},
 {"pid":1,"ts":12000000000,"dur":2000000000,"old":0,"new":50331648,"cpu":400000000},
 {"pid":1,"ts":14000000000,"dur":2000000000,"old":50331648,"new":0}],
 "samples":[
 {"pid":1,"ts":10000000000,"event":0},
 {"pid":1,"ts":11000000000,"event":0},
 {"pid":1,"ts":12000000000,"event":50331648},
 {"pid":1,"ts":13000000000,"event":50331648},
 {"pid":1,"ts":14000000000,"event":0}]}
JSON
generate "$TMP/equal" "$TMP/equal.json"
run "$TMP/equal" "$TMP/equal.out" 0
chk 'equal CPU A/B/C and raw ns' "$(row "$TMP/equal.out" CPU)" '50.000000 50.000000 50.000000 50.000000 50.000000 0.000000 0.000000 0.000000 0.000000 2000000000 2000000000 2000000000 2000000000 2000000000'
chk 'equal Lock A/B/C and raw ns' "$(row "$TMP/equal.out" relation)" '50.000000 50.000000 50.000000 50.000000 50.000000 0.000000 0.000000 0.000000 0.000000 2000000000 2000000000 2000000000 2000000000 2000000000'
chk 'equal totals' "$(row "$TMP/equal.out" TOTAL)" '100.000000 100.000000 100.000000 100.000000 100.000000 0.000000 0.000000 0.000000 0.000000 4000000000 4000000000 4000000000 4000000000 4000000000'
chk 'conservation measured' "$(field "$TMP/equal.out" ABC_SAMPLED_MEASURED_NS)" 4000000000
chk 'conservation nominal' "$(field "$TMP/equal.out" ABC_SAMPLES_NOMINAL_NS)" 4000000000
chk 'exact window' "$(field "$TMP/equal.out" ABC_EXACT_WINDOW_NS)" 4000000000
chk 'equal ratio' "$(field "$TMP/equal.out" ABC_MEASURED_NOMINAL_RATIO)" 1.000000000
chk 'interval CPU presence' "$(field "$TMP/equal.out" CPU_NS_PRESENT)" '1 1'
chk 'sample CPU presence' "$(field "$TMP/equal.out" ABC_JOINED_CPU_NS_PRESENT)" '2 2'
chk 'equal join closes' "$(field "$TMP/equal.out" JOIN_CLOSES)" 'yes 0'

# --abc alone uses the documented shipped 100ms period, not an inferred
# minimum recorded weight: four 1s samples / four 100ms weights = ratio 10.
rc=0
"$XVAL" "$TMP/equal" --abc > "$TMP/default.out" 2>&1 || rc=$?
chk 'ABC with default nominal period exit' "$rc" 0
chk 'default nominal total' "$(field "$TMP/default.out" ABC_SAMPLES_NOMINAL_NS)" 400000000
chk 'default nominal ratio' "$(field "$TMP/default.out" ABC_MEASURED_NOMINAL_RATIO)" 10.000000000
chk 'default nominal shares and totals' "$(row "$TMP/default.out" TOTAL)" '100.000000 100.000000 100.000000 100.000000 100.000000 0.000000 0.000000 0.000000 0.000000 4000000000 4000000000 4000000000 4000000000 400000000'

# Same exact timeline, but only two samples: CPU at 10s weighs 1s and a
# delayed Lock sample at 13s weighs 3s. Thus A=C_measured=25/75, B=C_nominal
# =50/50. C_measured-C_nominal is exactly -25/+25 pp; C-B is the same.
# gen_test_traces uses one weight per JSON scenario, so produce two raw files
# with different weights. Their header clock anchors retain wall==mono.
python3 - "$TMP" <<'PY'
import json, sys
from pathlib import Path
p = Path(sys.argv[1])
d = json.loads((p/'equal.json').read_text())
d['samples'] = d['samples'][:1]
(p/'early.json').write_text(json.dumps(d))
d = {'sample_period_ns': 3000000000, 'samples': [
    {'pid': 1, 'ts': 13000000000, 'event': 50331648},
    {'pid': 1, 'ts': 14000000000, 'event': 0}]}
(p/'late.json').write_text(json.dumps(d))
PY
generate "$TMP/early" "$TMP/early.json"
generate "$TMP/late" "$TMP/late.json"
mkdir "$TMP/weighted"
cp "$TMP/early/current.trace" "$TMP/weighted/2026-01-01_00.trace.lz4"
cp "$TMP/late/current.trace" "$TMP/weighted/2026-01-01_01.trace.lz4"
run "$TMP/weighted" "$TMP/weighted.out" 0
chk 'weighted CPU' "$(row "$TMP/weighted.out" CPU)" '25.000000 25.000000 50.000000 25.000000 50.000000 0.000000 0.000000 -25.000000 -25.000000 1000000000 1000000000 2000000000 1000000000 1000000000'
chk 'weighted Lock' "$(row "$TMP/weighted.out" relation)" '75.000000 75.000000 50.000000 75.000000 50.000000 0.000000 0.000000 25.000000 25.000000 3000000000 3000000000 2000000000 3000000000 1000000000'
chk 'weighted totals' "$(row "$TMP/weighted.out" TOTAL)" '100.000000 100.000000 100.000000 100.000000 100.000000 0.000000 0.000000 0.000000 0.000000 4000000000 4000000000 4000000000 4000000000 2000000000'
chk 'measured/nominal ratio (reported, not asserted by tool)' "$(field "$TMP/weighted.out" ABC_MEASURED_NOMINAL_RATIO)" 2.000000000
chk 'weighted join closes' "$(field "$TMP/weighted.out" JOIN_CLOSES)" 'yes 0'

# Two unjoined instants: pid 1 at 11s is in a HOLE [11,12), while pid 2 at
# 13s has no timeline even though pid 1 has a covering interval. A=2s CPU,
# 2s Lock. B=1s CPU,2s Lock. Joined C=1s CPU,1s Lock. This also proves the
# lookup includes pid and does not silently attach another backend's state.
python3 - "$TMP" <<'PY'
import json, sys
from pathlib import Path
p = Path(sys.argv[1]); d = json.loads((p/'equal.json').read_text())
d['events'][1]['ts'] = 11000000000
d['events'][1]['dur'] = 1000000000
d['samples'][3]['pid'] = 2
(p/'hole.json').write_text(json.dumps(d))
PY
generate "$TMP/hole" "$TMP/hole.json"
run "$TMP/hole" "$TMP/hole.out" 0
chk 'unjoined count and measured ns' "$(field "$TMP/hole.out" UNJOINED)" '2 2000000000'
chk 'hole CPU' "$(row "$TMP/hole.out" CPU)" '50.000000 50.000000 33.333333 50.000000 50.000000 0.000000 0.000000 16.666667 0.000000 2000000000 1000000000 1000000000 1000000000 1000000000'
chk 'hole totals' "$(row "$TMP/hole.out" TOTAL)" '100.000000 100.000000 100.000000 100.000000 100.000000 0.000000 0.000000 0.000000 0.000000 4000000000 2000000000 3000000000 2000000000 2000000000'
chk 'hole join closes' "$(field "$TMP/hole.out" JOIN_CLOSES)" 'yes 0'

# Exact-tier coverage regression: pids 1 and 2 each close CPU [10,14).
# Pid 3 enters Lock at 10s and NEVER closes that interval. Its four samples
# correctly read Lock, but cannot join. All twelve in-window samples weigh
# 1s: A CPU=8s, Lock=4s (2/3,1/3), A_joined CPU=8s, Lock=0 (1,0).
# B CPU=8s, Lock=0; both Cs CPU=8s, Lock=0. Thus CPU coverage is -100/3 pp,
# Lock coverage +100/3 pp, and reading/classification is ZERO for both.
# --min-share 0 displays Lock even though its exact-tier share is zero.
cat > "$TMP/open-lock.json" <<'JSON'
{"sample_period_ns":1000000000,"interleave":1,"events":[
 {"pid":1,"ts":10000000000,"dur":1000000000,"old":0,"new":0},
 {"pid":2,"ts":10000000000,"dur":1000000000,"old":0,"new":0},
 {"pid":3,"ts":10000000000,"dur":1000000000,"old":0,"new":50331648},
 {"pid":1,"ts":14000000000,"dur":4000000000,"old":0,"new":0},
 {"pid":2,"ts":14000000000,"dur":4000000000,"old":0,"new":0}],
 "samples":[
 {"pid":1,"ts":10000000000,"event":0},
 {"pid":2,"ts":10000000000,"event":0},
 {"pid":3,"ts":10000000000,"event":50331648},
 {"pid":1,"ts":11000000000,"event":0},
 {"pid":2,"ts":11000000000,"event":0},
 {"pid":3,"ts":11000000000,"event":50331648},
 {"pid":1,"ts":12000000000,"event":0},
 {"pid":2,"ts":12000000000,"event":0},
 {"pid":3,"ts":12000000000,"event":50331648},
 {"pid":1,"ts":13000000000,"event":0},
 {"pid":2,"ts":13000000000,"event":0},
 {"pid":3,"ts":13000000000,"event":50331648},
 {"pid":1,"ts":14000000000,"event":0}]}
JSON
generate "$TMP/open-lock" "$TMP/open-lock.json"
run "$TMP/open-lock" "$TMP/open-lock.out" 0 --min-share 0
chk 'open Lock unjoined count and measured ns' "$(field "$TMP/open-lock.out" UNJOINED)" '4 4000000000'
chk 'open Lock CPU gap is coverage, reading is zero' "$(row "$TMP/open-lock.out" CPU)" '66.666667 100.000000 100.000000 100.000000 100.000000 -33.333333 0.000000 0.000000 0.000000 8000000000 8000000000 8000000000 8000000000 8000000000'
chk 'open Lock relation gap is coverage, reading is zero' "$(row "$TMP/open-lock.out" relation)" '33.333333 0.000000 0.000000 0.000000 0.000000 33.333333 0.000000 0.000000 0.000000 4000000000 0 0 0 0'
chk 'open Lock totals conserve joined A = A minus unjoined' "$(row "$TMP/open-lock.out" TOTAL)" '100.000000 100.000000 100.000000 100.000000 100.000000 0.000000 0.000000 0.000000 0.000000 12000000000 8000000000 8000000000 8000000000 8000000000'
chk 'open Lock join closes' "$(field "$TMP/open-lock.out" JOIN_CLOSES)" 'yes 0'
chk 'open Lock decomposition closes' "$(field "$TMP/open-lock.out" ABC_DECOMPOSITION_CLOSES)" yes
chk 'decomposition identity' "$(field "$TMP/open-lock.out" ABC_DECOMPOSITION_IDENTITY)" 'A-B = (A-A_joined)+(A_joined-C_measured)+(C_measured-B)'
chk 'ABC column names' "$(field "$TMP/open-lock.out" ABC_COLUMNS)" 'event A A_joined B C_measured C_nominal A-A_joined A_joined-C_measured C_measured-B C_measured-C_nominal A_ns A_joined_ns B_ns C_measured_ns C_nominal_ns (shares/deltas in pp)'
chk 'ABC delta labels' "$(field "$TMP/open-lock.out" ABC_DELTAS)" 'A-A_joined: coverage gap (exact-tier); A_joined-C_measured: reading and classification; C_measured-B: selection/weighting; C_measured-C_nominal: weighting'

# Classification mismatch: [10,11) is command-gated CPU, [11,12) ClientRead,
# [12,13) Lock. Recorded samples are CPU, Lock, ClientRead. A DB=2s (50/50),
# B DB=1s Lock, C DB=1s Lock, C excluded=2s, A excluded=1s. All THREE sample
# instants must be joined, including recorded idle. Reconciliation is
# C_DB(1)+C_excluded(2) = A_DB(2)+A_excluded(1)-UNJOINED(0), in seconds.
# CMD_END at 10s establishes seen_cmd without opening a command. Markers
# and zero-clipped boundary intervals must not count as CPU-present intervals.
cat > "$TMP/classification.json" <<'JSON'
{"sample_period_ns":1000000000,"events":[
 {"pid":1,"ts":10000000000,"old":4294967287,"new":0},
 {"pid":1,"ts":11000000000,"dur":1000000000,"old":0,"new":100663296},
 {"pid":1,"ts":12000000000,"dur":1000000000,"old":100663296,"new":50331648},
 {"pid":1,"ts":13000000000,"dur":1000000000,"old":50331648,"new":0}],
 "samples":[
 {"pid":1,"ts":10000000000,"event":0},
 {"pid":1,"ts":11000000000,"event":50331648},
 {"pid":1,"ts":12000000000,"event":100663296},
 {"pid":1,"ts":13000000000,"event":0}]}
JSON
generate "$TMP/classification" "$TMP/classification.json"
run "$TMP/classification" "$TMP/classification.out" 0
chk 'command gate / idle / A-C' "$(row "$TMP/classification.out" relation)" '50.000000 50.000000 100.000000 100.000000 100.000000 0.000000 -50.000000 0.000000 0.000000 1000000000 1000000000 1000000000 1000000000 1000000000'
chk 'A excluded idle' "$(field "$TMP/classification.out" ABC_A_EXCLUDED_NS)" 1000000000
chk 'joined A keeps recorded idle classification' "$(field "$TMP/classification.out" ABC_A_JOINED_EXCLUDED_NS)" 1000000000
chk 'C excluded idle and gated CPU' "$(field "$TMP/classification.out" ABC_C_EXCLUDED_NS)" '2000000000 2000000000'
chk 'all legacy intervals UNKNOWN' "$(field "$TMP/classification.out" CPU_NS_PRESENT)" '0 3'
chk 'excluded samples still joined' "$(field "$TMP/classification.out" UNJOINED)" '0 0'
chk 'classification join closes' "$(field "$TMP/classification.out" JOIN_CLOSES)" 'yes 0'
run "$TMP/equal" "$TMP/floor.out" 0 --min-share 51
chk 'floor hides event rows, keeps total' "$(awk '/^ABC_ROW /{n++} END{print n+0}' "$TMP/floor.out")" 1

# Frozen verbatim from the PRE-CHANGE binary (cross_validate.c blob
# 8ebd794365d2a385304e5937a481118d572b2e49), on equal.json with these flags.
# Only the temporary path in the warning is interpolated. No git-history or
# old-binary dependency: the oracle survives shallow clones and later edits.
cat > "$TMP/before.out" <<EOF
WARN: $TMP/equal has no wait_event_names.json sidecar -- using the PG18 tables recorded in the trace header. Names are this build's static tables, not the ones the trace was written with.
Overlap window: 4.0s (sampled + exact both present)

wait_event                   exact_share%     sampled%   delta_pp
------------------------------------------------------------------
CPU                                 50.0%        50.0%       0.0
relation                            50.0%        50.0%       0.0

Samples in window: 4
Top-2 event overlap: 2/2
Max share disagreement (events >= 2% exact share): 0.0 pp (n/a)
Tolerance: +/- 100.0 pp

=== IDLE: NOT DB Time (excluded from every share above) ===
IDLE_PACING_MASK 0x29B  settled_pg_major=18  sidecar=no
IDLE_EVENTS none -- mask resolved, and this trace carries no idle event on either tier
IDLE_EVENT_ROWS 0
IDLE_TOTAL_EXACT_NS 0
IDLE_TOTAL_SAMPLED_NS 0
IDLE_SAMPLES 0
IDLE_EXACT_PCT_OF_DBTIME 0.00
IDLE_SAMPLED_PCT_OF_DBTIME 0.00

RAW_WINDOW_S 4.000000
RAW_SAMPLE_PERIOD_NS 1000000000
RAW_TOTAL_EXACT_NS 4000000000
RAW_TOTAL_SAMPLED_NS 4000000000
RAW_AAS_EXACT 1.000000
RAW_AAS_SAMPLED 1.000000
RAW_CPU_EXACT_NS 2000000000
RAW_CPU_SAMPLED_NS 2000000000
RAW_NONCPU_EXACT_NS 2000000000
RAW_NONCPU_SAMPLED_NS 2000000000
RAW_CPU_SAMPLES 2
RAW_NONCPU_SAMPLES 2
RAW_GATE_REJECTED_EXACT_CPU_NS 0
RAW_GATE_REJECTED_INTERVALS 0
RAW_SAMPLED_CPU_NS_NEEDED_FOR_PARITY 0
RAW_SAMPLED_CPU_SAMPLES_NEEDED_FOR_PARITY 0
On-CPU-during-wait (spin): n/a (no measured cpu_ns — legacy/v2 trace)

RESULT: PASS
EOF
rc=0
"$XVAL" "$TMP/equal" --raw-ns --show-idle --tolerance 100 > "$TMP/plain.out" 2>&1 || rc=$?
chk 'flag-off exit' "$rc" 0
diff -u "$TMP/before.out" "$TMP/plain.out"
chk 'flag-off bytes match previous binary' yes yes
chk 'flag-off has no ABC lines' "$(awk '/ABC/{n++} END{print n+0}' "$TMP/plain.out")" 0
run "$TMP/equal" "$TMP/opt-in.out" 0 --raw-ns --show-idle
sed '/^=== ABC ===/,$d' "$TMP/opt-in.out" > "$TMP/stripped.out"
# One blank line separates the appended block from the previous output.
sed '$d' "$TMP/stripped.out" > "$TMP/stripped-trimmed.out"
diff -u "$TMP/plain.out" "$TMP/stripped-trimmed.out"
chk 'ABC leaves all previous output bytes unchanged' yes yes

# Negative control: compile a temporary mutant of the actual instrument,
# dropping ONE ns at every C_measured deposit. On equal.json four samples
# lose 4ns, so the independently summed residual MUST be -4 and exit MUST be
# 5. No runtime fault switch in the instrument, no fabricated output string.
python3 - "$TMP/mutant.c" <<'PY'
from pathlib import Path
import sys
s = Path('cross_validate.c').read_text()
anchor = 'c->c_measured_ns += sample->weight;'
assert s.count(anchor) == 1, 'negative-control mutation anchor missing or ambiguous'
Path(sys.argv[1]).write_text(s.replace(anchor, 'c->c_measured_ns += sample->weight - 1;'))
PY
cat > "$TMP/mutant.mk" <<'MAKE'
$(ABC_MUTANT): $(ABC_MUTANT).c $(BUILD)/server_event_reader.o \
    $(BUILD)/server_event_writer.o $(BUILD)/server_wait_event.o \
    $(BUILD)/server_idle_rule.o $(BUILD)/server_spawn.o $(BUILD)/server_cJSON.o
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^ -llz4 -lz
MAKE
test_cflags='-g -O2 -Wall -Werror=implicit-function-declaration -I../src'
if [[ $(uname -s) == Darwin ]]; then
    brew_prefix=$(brew --prefix)
    test_cflags="$test_cflags -I$brew_prefix/include -L$brew_prefix/lib"
fi
make -s -f Makefile -f "$TMP/mutant.mk" "ABC_MUTANT=$TMP/mutant" \
    "CFLAGS=${ABC_TEST_CFLAGS:-$test_cflags}" "$TMP/mutant"
original_xval=$XVAL
XVAL="$TMP/mutant"
run "$TMP/equal" "$TMP/negative.out" 5
XVAL=$original_xval
chk 'negative control join fails' "$(field "$TMP/negative.out" JOIN_CLOSES)" 'no -4'
chk 'negative control reports unexplained residual' "$(field "$TMP/negative.out" JOIN_RESIDUAL_UNEXPLAINED)" -4
chk 'negative control suppresses ABC shares' "$(awk '/^ABC_ROW /{n++} END{print n+0}' "$TMP/negative.out")" 0
tail -2 "$TMP/negative.out"

# Independently lose 1ns from each A_joined bucket deposit. C still balances
# perfectly, so JOIN_CLOSES must also check A_joined conservation itself.
python3 - "$TMP/aj-mutant.c" <<'PY'
from pathlib import Path
import sys
s = Path('cross_validate.c').read_text()
anchor = 'acc_find(table, sample->event)->a_joined_ns += sample->weight;'
assert s.count(anchor) == 1, 'A_joined mutation anchor missing or ambiguous'
Path(sys.argv[1]).write_text(s.replace(anchor, anchor.replace('sample->weight;', 'sample->weight - 1;')))
PY
make -s -f Makefile -f "$TMP/mutant.mk" "ABC_MUTANT=$TMP/aj-mutant" \
    "CFLAGS=${ABC_TEST_CFLAGS:-$test_cflags}" "$TMP/aj-mutant"
XVAL="$TMP/aj-mutant"
run "$TMP/equal" "$TMP/aj-negative.out" 5
XVAL=$original_xval
chk 'A_joined negative control join fails' "$(field "$TMP/aj-negative.out" JOIN_CLOSES)" 'no -4'
chk 'A_joined negative control residual' "$(field "$TMP/aj-negative.out" JOIN_RESIDUAL_A_JOINED)" -4
chk 'A_joined negative control reports unexplained residual' "$(field "$TMP/aj-negative.out" JOIN_RESIDUAL_UNEXPLAINED)" -4
chk 'A_joined negative control suppresses ABC shares' "$(awk '/^ABC_ROW /{n++} END{print n+0}' "$TMP/aj-negative.out")" 0
tail -2 "$TMP/aj-negative.out"

echo "test_cross_validate_abc: $pass passed, 0 failed"
