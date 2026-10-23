#!/bin/bash
cd /root/t
D=/var/lib/postgresql/pgwet
P="/opt/pgwet/bin/psql -X -q -h /var/run/postgresql -p 5419 -U postgres"
cap() { $P -d postgres -c "alter database shop set pg_wait_event_tracing.capture = '$1'" >/dev/null; }
restart() { # restart SB LIMIT(yes|no)
  systemctl stop pgwet 2>/dev/null; systemctl reset-failed pgwet 2>/dev/null; sleep 2; sed -i "s/^shared_buffers = .*/shared_buffers = '$1'/" $D/postgresql.conf
  if [ $2 = yes ]; then systemd-run -q --unit=pgwet --uid=postgres --gid=postgres -p MemoryMax=1200M -p KillMode=mixed /opt/pgwet/bin/postgres -D $D
  else systemd-run -q --unit=pgwet --uid=postgres --gid=postgres -p KillMode=mixed /opt/pgwet/bin/postgres -D $D; fi
  sleep 4; sync; echo 3 > /proc/sys/vm/drop_caches; }
cap off; ./sh.sh R0b 20 pgws full | tail -1
cap trace; ./sh.sh R1 20 patch full | tail -1
cap off; $P -d postgres -c "alter system set synchronous_commit = off" -c "select pg_reload_conf()" >/dev/null
./sh.sh F1a 20 pgws full | tail -1
$P -d postgres -c "alter system reset synchronous_commit" -c "select pg_reload_conf()" >/dev/null
./sh.sh F2 20 pgws chunked | tail -1
restart 2GB no; $P -d shop -tAc "select pg_prewarm('catalog'), pg_prewarm('catalog_pkey'), pg_prewarm('orders'), pg_prewarm('orders_item_id_idx')" > /dev/null
./sh.sh F1b 20 pgws full | tail -1
restart 128MB yes
echo BATCH_DONE
