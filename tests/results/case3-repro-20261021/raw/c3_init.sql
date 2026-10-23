create table t (id bigint primary key, grp int, filler text) with (fillfactor = 100);
insert into t select g, g % 1000, repeat(md5(g::text), 6) from generate_series(1, 12000000) g;
vacuum analyze t;
alter database case3 set track_io_timing = on;
create extension if not exists pg_stat_statements;
select pg_size_pretty(pg_table_size($$t$$)) as heap, pg_size_pretty(pg_indexes_size($$t$$)) as idx;
