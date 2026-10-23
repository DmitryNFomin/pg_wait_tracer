-- E5 worker: mode 'long' (50 ms sleeps) or 'short' (0.5 ms sleeps), for 30 s of wall time.
-- psql does not substitute variables inside dollar quotes, so pass them as settings.
SELECT set_config('e5.mode', :'mode', false);
DO $do$
DECLARE m text := current_setting('e5.mode');
        d float8 := CASE WHEN current_setting('e5.mode') = 'long' THEN 0.05 ELSE 0.0005 END;
        t_end timestamptz := clock_timestamp() + interval '30 s';
        n int := 0; total interval := '0'; t0 timestamptz;
BEGIN
  WHILE clock_timestamp() < t_end LOOP
    t0 := clock_timestamp(); PERFORM pg_sleep(d); total := total + (clock_timestamp() - t0); n := n + 1;
  END LOOP;
  INSERT INTO e5_result(mode, pid, waits, sleep_s) VALUES (m, pg_backend_pid(), n, extract(epoch from total));
END $do$;
