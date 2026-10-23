-- Q: pure CPU victim (wait_event_info stays 0) for :secs seconds.
SET application_name = 'qvictim';
SELECT set_config('q.secs', :'secs', false);
DO $do$
DECLARE t_end timestamptz := clock_timestamp() + make_interval(secs => current_setting('q.secs')::float8);
        x bigint := 0;
BEGIN
  WHILE clock_timestamp() < t_end LOOP x := x + 1; END LOOP;
END $do$;
