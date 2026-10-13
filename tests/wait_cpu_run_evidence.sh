#!/usr/bin/env bash
# Reproduce the raw per-class wait-CPU scan (criteria item 2.5).
#
#   bash tests/wait_cpu_run_evidence.sh TRACE_DIR [OUT_DIR]
#
# Exit status is the scanner's: 0 CLEAN, 2 REFUSED, 3 CROSSING, 4 UNMEASURED.
# The independent record census comes from `pgwt-server --dump`; if that
# dependency cannot run (missing, not executable, exit 126/127) or its census
# line cannot be parsed, this script FAILS. It never falls back to running the
# scan with the completeness check switched off — a dependency that cannot be
# executed is exactly how a coverage gate ends up approving without looking.
#
# LIVES IN tests/, NOT IN tests/results/: box-check.sh's up-rsync excludes
# `tests/results` (scripts/box-check.sh), so while this script sat in the
# evidence directory it did not exist on the gate box at all -- and the two
# cases of tests/test_wait_cpu_scan.py that drive it failed there with exit
# 127, "No such file or directory". That is the broken-invocation failure
# CLAUDE.md calls out, not the 126/127 census behaviour those cases exist to
# prove. The evidence directory holds artifacts; a script a registered test
# depends on has to ship with the tree.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Artifacts and the prebuilt binaries still default into the evidence dir;
# only this script moved.
EVIDENCE="$ROOT/tests/results/issue25-perclass-raw-scan"
TRACE="${1:?provide the retained trace directory}"
OUT="${2:-$EVIDENCE}"
BIN="${PGWT_SCAN_BIN_DIR:-$EVIDENCE/bin}"
THRESHOLD_NS="${THRESHOLD_NS:-824430}"   # 10x the measured sleep-class p99
PURE_SLEEP="${PURE_SLEEP:-Timeout:PgSleep,Lock:relation,Lock:advisory}"
mkdir -p "$OUT"

if [[ "${PGWT_SKIP_BUILD:-0}" != 1 ]]; then
    # Refuse clearly rather than reporting a build failure that is really an
    # absent builder: the evidence dir is not synced to the gate box, so a
    # build has to be asked for where it can actually happen.
    if [[ ! -r "$EVIDENCE/build.sh" ]]; then
        echo "cannot build: $EVIDENCE/build.sh is not present (tests/results is" >&2
        echo "excluded from box-check's rsync). Pass PGWT_SKIP_BUILD=1 with" >&2
        echo "PGWT_SCAN_BIN_DIR pointing at prebuilt binaries, or run this" >&2
        echo "from a full checkout." >&2
        exit 2
    fi
    bash "$EVIDENCE/build.sh" "$BIN" > "$OUT/build.log" 2>&1 || {
        echo "build failed; see $OUT/build.log" >&2; exit 2; }
fi

"$BIN/pgwt-server" --trace-dir "$TRACE" --dump > "$OUT/dump.txt" 2> "$OUT/dump.stderr.txt"
dump_rc=$?
if [[ $dump_rc -ne 0 ]]; then
    echo "census dependency pgwt-server --dump failed (exit $dump_rc)" >&2
    exit 2
fi

census="$(sed -n 's/.*Events: [0-9]* (\([0-9]*\) transitions, \([0-9]*\) samples).*/\1 \2/p' \
          "$OUT/dump.txt" | head -1)"
if [[ -z "$census" ]]; then
    echo "could not parse the record census from $OUT/dump.txt" >&2
    exit 2
fi
read -r EXPECT_TRANSITIONS EXPECT_SAMPLES <<< "$census"
echo "independent census: transitions=$EXPECT_TRANSITIONS samples=$EXPECT_SAMPLES"

"$BIN/wait_cpu_scan" --trace-dir "$TRACE" --threshold-ns "$THRESHOLD_NS" \
    --expect-transitions "$EXPECT_TRANSITIONS" \
    --expect-samples "$EXPECT_SAMPLES" \
    --pure-sleep "$PURE_SLEEP" \
    --output "$OUT/measurement.json" 2> "$OUT/measurement.stderr.txt"
scan_rc=$?
cat "$OUT/measurement.stderr.txt"
printf 'dump_exit=%s\nscan_exit=%s\n' "$dump_rc" "$scan_rc" > "$OUT/exit-status.txt"
exit "$scan_rc"
