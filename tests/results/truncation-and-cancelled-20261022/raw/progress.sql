SELECT extract(epoch from clock_timestamp())::numeric(20,3), p.pid, p.phase, p.heap_blks_total, p.heap_blks_scanned, p.heap_blks_vacuumed
FROM pg_stat_progress_vacuum p \watch 0.2
