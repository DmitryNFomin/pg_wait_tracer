#!/bin/bash
# Case 5 on the patched build: TPC-B at 1,400 tx/s + parallel report every ~5 s; CPU limit none / 1 CPU / 2 CPU.
cd /root/t
for i in $(seq 7200); do grep -q C4-ALLDONE c4batch.log && break; sleep 5; done
export PGOPTIONS="-c pg_wait_event_tracing.capture=off"
P="/opt/pgwet/bin/psql -X -q -h /var/run/postgresql -p 5419 -U postgres"
$P -d c3 -tAc "select pg_terminate_backend(pid) from pg_stat_activity where application_name='reporting_job'"
pkill -f "sleep 7200"
systemctl set-property --runtime pgwet.service MemoryMax=infinity
$P -d postgres -c "vacuum analyze" -c checkpoint
unset PGOPTIONS
export REPORT_RATE=0.2
for r in P0o:none:off P1o:100%:off P1t:100%:trace P0t:none:trace P7t:200%:trace; do IFS=: read tag lim cap <<< "$r"; ./c5.sh $tag $lim 1400 $cap; done
echo C5-ALLDONE
