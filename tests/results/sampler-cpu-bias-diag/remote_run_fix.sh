#!/bin/bash
# Second half: rebuild with the recheck enabled and rerun the same two captures.
set -uo pipefail
R=pgwt-check/agent_sampler-cpu-bias-diag
cd "/root/$R" || exit 1
exec 9>/tmp/pgwt-box-check.lock
echo "DIAG(fix) waiting for lock at $(date -Is)"
flock -w 3600 9 || { echo "DIAG-ERROR: flock timed out"; exit 1; }
echo "DIAG(fix) lock acquired at $(date -Is)"
grep -n 'd->debug_query_string_addr) {' src/sampler.c | head -3
make -j"$(nproc)" >/tmp/diag-build-fix.log 2>&1 || { echo "DIAG-ERROR: make failed"; tail -30 /tmp/diag-build-fix.log; exit 1; }
make -C tests cross_validate >>/tmp/diag-build-fix.log 2>&1 || { echo "DIAG-ERROR: cross_validate build failed"; exit 1; }
echo "DIAG(fix) build ok"
for RATE in 10 200; do
  echo "================ FIX rate=${RATE} ================"
  bash /root/diag_sampler_cpu_bias.sh "$RATE" "/root/diag-out/fix-$RATE" "/root/$R" 2>&1
  echo "DIAG capture exit=$?"
done
echo "DIAG(fix) done at $(date -Is)"
