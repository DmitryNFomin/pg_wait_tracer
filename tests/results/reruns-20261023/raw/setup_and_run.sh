#!/bin/bash
# Waits for the build, creates the patched server (same config as the 2026-10-22 runs), loads c3 and shop, runs rerun.sh.
exec > /root/t/all.log 2>&1
until grep -q BUILD-DONE /root/wetbuild.log; do sleep 10; done
git -C /root/pgwet log -1 --format='wet-v12 %h %s'
systemctl stop postgresql@18-main
D=/var/lib/postgresql/pgwet; rm -rf $D; mkdir -p $D; chown postgres:postgres $D
su postgres -c "/opt/pgwet/bin/initdb -D $D -U postgres >/dev/null"
cat >> $D/postgresql.conf <<'C'
port = 5419
unix_socket_directories = '/var/run/postgresql'
shared_buffers = '128MB'
shared_preload_libraries = 'pg_stat_statements,pg_wait_sampling,pg_wait_event_tracing'
compute_query_id = on
track_io_timing = on
log_lock_waits = on
log_line_prefix = '%m [%p] %a '
logging_collector = off
max_connections = 100
C
mkdir -p /var/run/postgresql; chown postgres:postgres /var/run/postgresql
systemd-run --unit=pgwet --uid=postgres --gid=postgres -p KillMode=mixed -p MemoryMax=1200M /opt/pgwet/bin/postgres -D $D
sleep 4
P="/opt/pgwet/bin/psql -X -q -h /var/run/postgresql -p 5419 -U postgres"
cd /root/t
$P -d postgres -c "create database c3" -c "create database shop"
$P -d c3 -f c2_init.sql
$P -d shop -f shop_init.sql
$P -d shop -c "create extension if not exists pg_wait_event_tracing"
$P -d postgres -tAc "select name, setting from pg_settings where name in ('shared_buffers','io_method','pg_wait_event_tracing.trace_ring_size','pg_wait_event_tracing.capture')"
systemctl show -p MemoryMax pgwet
echo SETUP-DONE
sync; echo 3 > /proc/sys/vm/drop_caches
./rerun.sh
