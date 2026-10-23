#!/bin/bash
# rerun.sh: (1) case 1 F2 repeats: baseline R0 and chunked restock F2 alternated, 3 each (capture off, pg_wait_sampling only).
# (2) case 4 bloat cost check: statement_timeout 10 ms, capture off / trace without dumps during the window / trace with
#     5 s dumps, 3 rounds, order rotated per round. Same server: wet-v12, shared_buffers 128MB, MemoryMax=1200M.
cd /root/t
P="/opt/pgwet/bin/psql -X -q -h /var/run/postgresql -p 5419 -U postgres"
dc() { sync; echo 3 > /proc/sys/vm/drop_caches; sleep 2; }
for i in 1 2 3; do ./sh.sh R0-$i 20 pgws full | tail -1; ./sh.sh F2-$i 20 pgws chunked | tail -1; done
echo C1-DONE
./bloat.sh > bloat.log 2>&1 &
for i in $(seq 1800); do grep -q BLOAT_DONE bloat.log && break; sleep 2; done
for r in 1 2 3; do
  case $r in 1) ORD="o n t";; 2) ORD="n t o";; 3) ORD="t o n";; esac
  for m in $ORD; do dc
    case $m in o) ./c4.sh bl-o10-$r 200000 10 off;; n) ./c4.sh bl-n10-$r 200000 10 trace none;; t) ./c4.sh bl-t10-$r 200000 10 trace every5;; esac
  done
done
$P -d c3 -c "alter database c3 set pg_wait_event_tracing.capture = 'off'"
echo ALL-DONE
