-- E7 pg_stat_activity poller at ~100 Hz, the way SQL-based ASH scripts sample.
SELECT set_config('e7.secs', :'secs', false), set_config('e7.run', :'run', false);
DO $do$
DECLARE t_end timestamptz := clock_timestamp() + make_interval(secs => current_setting('e7.secs')::float8);
BEGIN
  WHILE clock_timestamp() < t_end LOOP
    PERFORM pg_stat_clear_snapshot();
    INSERT INTO e7_psa SELECT current_setting('e7.run'), clock_timestamp(), pid, state, wait_event_type, wait_event
      FROM pg_stat_activity WHERE application_name = 'e7victim';
    PERFORM pg_sleep(0.01);
  END LOOP;
END $do$;
