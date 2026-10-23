#!/bin/bash
# E2: actual cadence of pg_wait_sampling's collector (history_period 10 ms) under load.
set -uo pipefail
cd /root/sampler-e2; source ./common.sh
run() {
  local tag=$1; shift
  if [ $# -gt 0 ]; then $PGB "$@" -T 80 > pgb-$tag.txt 2>&1 & PB=$!; sleep 10; fi
  T0=$(now_ts); sleep 60; T1=$(now_ts)
  [ $# -gt 0 ] && wait $PB
  sleep 2
  $P -tAc "copy (select distinct extract(epoch from ts)::numeric(20,6) from pg_wait_sampling_history where ts >= '$T0' and ts < '$T1' order by 1) to stdout" > rounds-$tag.txt
  echo "$tag rounds=$(wc -l < rounds-$tag.txt) $(grep -E '^tps' pgb-$tag.txt 2>/dev/null | head -1)"
  cat /proc/loadavg
}
run idle
run sel-c4  -S -c 4 -j 4
run sel-c8  -S -c 8 -j 4
run sel-c16 -S -c 16 -j 4
run tpcb-c8 -c 8 -j 4
run tpcb-c16 -c 16 -j 4
