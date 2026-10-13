#!/usr/bin/env bash
set -euo pipefail
cd /root/cx-matrix-regression
exec 9>/tmp/pgwt-box-check.lock
flock -x 9
echo 'Acquired lock for raw-primed endpoint comparison'
mkdir -p audit/edge-first
mv audit/measure.jsonl audit/edge-first/
for arm in before329 after329 after340 pre after365 before_refusal after_refusal aggregate final oldsort untuned; do
    mv "audit/$arm-0.stderr" "audit/$arm-1.stderr" audit/edge-first/
done
python3 audit_measure.py >audit/measure.jsonl 9>&-
date -u >>audit/host.txt
echo 'Raw-primed endpoint comparison complete'
