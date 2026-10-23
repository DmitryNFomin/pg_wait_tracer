#!/bin/bash
# Case 4 on the patched build: storage (fio on the same disk) then bloat; capture off vs trace; no timeout vs 10 ms.
cd /root/t
dc() { sync; echo 3 > /proc/sys/vm/drop_caches; sleep 2; }
fio --name=noisy --filename=/root/fio.dat --size=4G --rw=randrw --rwmixread=50 --bs=64k --direct=1 --iodepth=32 \
    --numjobs=4 --ioengine=libaio --time_based --runtime=7200 --group_reporting > fio-storage.txt 2>&1 & FIO=$!
sleep 20
for r in st-o0:0:off st-t0:0:trace st-o10:10:off st-t10:10:trace; do IFS=: read tag tmo cap <<< "$r"; dc; ./c4.sh $tag 12000000 $tmo $cap; done
kill $FIO; wait $FIO 2>/dev/null; rm -f /root/fio.dat
/opt/pgwet/bin/psql -X -q -h /var/run/postgresql -p 5419 -U postgres -d c3 -c "alter database c3 set pg_wait_event_tracing.capture = 'off'"
./bloat.sh > bloat.log 2>&1 &
for i in $(seq 1800); do grep -q BLOAT_DONE bloat.log && break; sleep 2; done
for r in bl-o0:0:off bl-t0:0:trace bl-o10:10:off bl-t10:10:trace; do IFS=: read tag tmo cap <<< "$r"; dc; ./c4.sh $tag 200000 $tmo $cap; done
echo C4-ALLDONE
