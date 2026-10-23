P="psql -X -q -h /var/run/postgresql -p 5418 -U postgres -d postgres"
PGB="pgbench -h /var/run/postgresql -p 5418 -U postgres -d postgres"
now_ts() { $P -tAc "select clock_timestamp()"; }
