#!/bin/bash
# Queue behind the gate-2 flock, then build and run the two diagnostic captures.
set -uo pipefail
R=pgwt-check/agent_sampler-cpu-bias-diag
cd "/root/$R" || exit 1
exec 9>/tmp/pgwt-box-check.lock
echo "DIAG waiting for /tmp/pgwt-box-check.lock at $(date -Is)"
flock -w 3600 9 || { echo "DIAG-ERROR: flock timed out after 3600s"; exit 1; }
echo "DIAG lock acquired at $(date -Is)"
make -j"$(nproc)" >/tmp/diag-build.log 2>&1 || { echo "DIAG-ERROR: make failed"; tail -30 /tmp/diag-build.log; exit 1; }
make -C tests cross_validate >>/tmp/diag-build.log 2>&1 || { echo "DIAG-ERROR: cross_validate build failed"; tail -30 /tmp/diag-build.log; exit 1; }
echo "DIAG build ok"
for RATE in 10 200; do
  echo "================ BASELINE rate=${RATE} ================"
  bash /root/diag_sampler_cpu_bias.sh "$RATE" "/root/diag-out/base-$RATE" "/root/$R" 2>&1
  echo "DIAG capture exit=$?"
done
echo "DIAG baseline done at $(date -Is)"
