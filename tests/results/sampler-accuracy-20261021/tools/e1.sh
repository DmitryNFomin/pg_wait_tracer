#!/bin/bash
# E1: bare 100 Hz timer lateness under pgbench loads on PG18 (port 5418).
set -uo pipefail
OUT=/root/sampler-e1; mkdir -p $OUT; cd $OUT
gcc -O2 -Wall -o ticklat ticklat.c || exit 1
PGB="pgbench -h /var/run/postgresql -p 5418 -U postgres -d postgres"
run() {   # tag, pgbench args (empty = idle)
  local tag=$1; shift
  if [ $# -gt 0 ]; then $PGB "$@" -T 75 > pgb-$tag.txt 2>&1 & P=$!; sleep 10; fi
  ./ticklat 10000 60 > lat-$tag.txt 2> ticks-$tag.txt
  [ $# -gt 0 ] && wait $P
  echo "$tag $(cat ticks-$tag.txt) $(grep -E '^tps' pgb-$tag.txt 2>/dev/null | head -1)"
}
run idle
run sel-c4  -S -c 4 -j 4
run sel-c8  -S -c 8 -j 4
run sel-c16 -S -c 16 -j 4
run tpcb-c8 -c 8 -j 4
