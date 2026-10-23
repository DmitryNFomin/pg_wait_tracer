-- E6 sessions: role 'holder' or 'victim', for secs seconds, rows tagged with run.
SELECT set_config('e6.role', :'role', false), set_config('e6.secs', :'secs', false), set_config('e6.run', :'run', false);
DO $do$
DECLARE t_end timestamptz := clock_timestamp() + make_interval(secs => current_setting('e6.secs')::float8);
        r text := current_setting('e6.run');
        d float8; t0 timestamptz; t1 timestamptz;
BEGIN
  IF current_setting('e6.role') = 'holder' THEN
    WHILE clock_timestamp() < t_end LOOP
      d := (ARRAY[0.0002,0.001,0.002,0.005,0.01,0.02])[1 + floor(random()*6)::int];
      PERFORM pg_advisory_lock(42); PERFORM pg_sleep(d); PERFORM pg_advisory_unlock(42);
      PERFORM pg_sleep(0.002 + random()*0.02);
    END LOOP;
  ELSE
    WHILE clock_timestamp() < t_end LOOP
      t0 := clock_timestamp(); PERFORM pg_advisory_lock(42); t1 := clock_timestamp();
      PERFORM pg_advisory_unlock(42);
      INSERT INTO e6_waits VALUES (r, pg_backend_pid(), t0, t1);
      PERFORM pg_sleep(random()*0.01);
    END LOOP;
  END IF;
END $do$;
