#!/bin/sh
# Regenerate every derived table in this directory from the committed corpus.
#
#   sh scripts/regen.sh            # from tests/results/ui_live_anchor_evidence
#
# Every number quoted in FRAME_SPACING_DRIFT_BOUND_MS's comment, in
# ui_live_smoke.py's tick_baseline comment, and in this branch's commit message
# comes out of these seven commands. `diff` the output against the committed
# tables to check nothing drifted.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
corpus="$here/corpus"
out="$here/tables"
mkdir -p "$out"

[ -d "$corpus" ] || { echo "no corpus at $corpus" >&2; exit 1; }
n=$(ls "$corpus" | wc -l | tr -d ' ')
[ "$n" -gt 0 ] || { echo "corpus at $corpus is EMPTY -- refusing to write tables that would look like a clean result" >&2; exit 1; }
echo "corpus: $n runs"

py=${PYTHON:-python3}
$py "$here/scripts/verify.py"     "$corpus" > "$out/01-claims-cliff-separation-f45.txt"
$py "$here/scripts/anticorr.py"   "$corpus" > "$out/02-anticorrelation-ttfp-vs-anchor.txt"
$py "$here/scripts/headroom.py"   "$corpus" > "$out/03-headroom-to-the-1000ms-clamp.txt"
$py "$here/scripts/excess.py"     "$corpus" > "$out/04-tick1-excess-by-tab.txt"
$py "$here/scripts/decompose.py"  $(ls -d "$corpus"/*/summary.json) > "$out/05-per-run-overview.txt"
$py "$here/scripts/cliff2.py"     "$corpus" > "$out/06-cliff-metric-per-tab-per-run.txt"
$py "$here/scripts/thennow.py"    "$corpus" > "$out/07-then-vs-now.txt"
$py "$here/scripts/pertick.py"    "$corpus" 1791385783 > "$out/08-per-tick-the-run-that-went-red.txt"

echo "wrote $(ls "$out" | wc -l | tr -d ' ') tables to $out"
