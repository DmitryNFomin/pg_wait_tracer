#!/usr/bin/env bash
# INSTRUMENTATION for #671: 3 interleaved OTHER/FIFO pairs on one host.
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
HOSTTAG="${1:?need host tag}"
OUT="${2:-$SCRIPT_DIR/../tests/results/671}"
mkdir -p "$OUT"
for i in 1 2 3; do
  for pol in other fifo; do
    echo "=== $(date -u +%FT%TZ) $HOSTTAG-$pol-$i ==="
    "$SCRIPT_DIR/probe.sh" --label "$HOSTTAG-$pol-$i" --policy "$pol" \
        --rate 10 --clients 8 --esc 300 --out "$OUT" || echo "RUN FAILED rc=$?"
  done
done
echo "=== ALL RUNS DONE $(date -u +%FT%TZ) ==="
/usr/bin/grep -h '^JSONL ' "$OUT"/*/summary.txt | sed 's/^JSONL //' > "$OUT/all.jsonl"
wc -l "$OUT/all.jsonl"
