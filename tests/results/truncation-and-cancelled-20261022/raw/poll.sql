SELECT extract(epoch from clock_timestamp())::numeric(20,6), a.pid, a.wait_event, b.blocker, ba.backend_type, left(ba.query, 40)
FROM pg_stat_activity a CROSS JOIN LATERAL unnest(pg_blocking_pids(a.pid)) AS b(blocker)
LEFT JOIN pg_stat_activity ba ON ba.pid = b.blocker
WHERE a.wait_event_type = 'Lock' \watch 0.005
