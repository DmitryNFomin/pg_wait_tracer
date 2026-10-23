-- E7 victim: alternates ~1 ms of CPU (busy loop) with pg_sleep(0.001), logging its own phase times.
SET application_name = 'e7victim';
SELECT set_config('e7.secs', :'secs', false), set_config('e7.run', :'run', false);
DO $do$
DECLARE t_start timestamptz := clock_timestamp();
        t_end timestamptz := clock_timestamp() + make_interval(secs => current_setting('e7.secs')::float8);
        t0 timestamptz; t1 timestamptz; t2 timestamptz;
        slp interval := '0'; n int := 0;
BEGIN
  WHILE clock_timestamp() < t_end LOOP
    t0 := clock_timestamp();
    WHILE clock_timestamp() < t0 + interval '1 ms' LOOP END LOOP;
    t1 := clock_timestamp();
    PERFORM pg_sleep(0.001);
    t2 := clock_timestamp();
    slp := slp + (t2 - t1); n := n + 1;
  END LOOP;
  INSERT INTO e7_truth VALUES (current_setting('e7.run'), pg_backend_pid(), t_start, clock_timestamp(),
                               extract(epoch from slp), n);
END $do$;
