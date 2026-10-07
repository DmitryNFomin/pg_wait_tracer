#!/bin/bash
# sweep294.sh  --  base vs fix, all four rates, one capture per cell.
set -u
BASE=/root/pgwt-check/master-base
FIX=/root/pgwt-check/agent_sampler-cmd-gate-order
XVAL_BIN=$BASE/tests/cross_validate   # ONE comparator for both trees
export XVAL_BIN
ROOT=/root/diag294
mkdir -p $ROOT
for rate in 10 50 100 200; do
  for tree in base fix; do
    repo=$BASE; [[ $tree == fix ]] && repo=$FIX
    o=$ROOT/$tree-$rate
    rm -rf "$o"
    echo "##### $tree @ ${rate}Hz #####"
    bash /root/diag294.sh "$rate" "$o" "$repo" > "$o.log" 2>&1
    rc=$?
    echo "exit=$rc"
    grep -E "^(CPU |Max share|RESULT|RAW_CPU_|RAW_TOTAL_|RAW_AAS_|PROC_CLIENT_TOTAL|METRIC|DIAG-ERROR)" "$o.log"
  done
done
