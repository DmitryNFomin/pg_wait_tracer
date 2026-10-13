#!/usr/bin/env bash
# Reproduce this standalone measurement; a REFUSED result exits 2.
# Usage: bash tests/results/perclass-wait-cpu/run_evidence.sh TRACE_DIR [OUTPUT_DIR]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
TRACE="${1:?provide the retained trace directory}"
OUT="${2:-$ROOT/tests/results/perclass-wait-cpu}"
mkdir -p "$OUT"
cd "$ROOT"
bash tests/results/perclass-wait-cpu/build_server.sh > "$OUT/build.log" 2>&1
python3 tests/results/perclass-wait-cpu/test_measurement.py > "$OUT/tests.txt" 2>&1
# Diagnostic threshold calibrated on the Oct 2 trace: 10 * 82443 ns p99.
# This is a measurement argument, not a registered rehearsal bound.
set +e
python3 tests/measure_wait_cpu.py --trace-dir "$TRACE" --threshold-ns 824430 \
    --output "$OUT/measurement.json" > "$OUT/measurement.stdout" 2> "$OUT/progress.log"
MEASURE_EXIT=$?
set -e
printf 'focused_tests_exit=0\nmeasurement_exit=%s\n' "$MEASURE_EXIT" > "$OUT/exit-status.txt"
exit "$MEASURE_EXIT"
