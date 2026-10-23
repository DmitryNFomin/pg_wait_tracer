COPY (SELECT extract(epoch from ts)::numeric(20,6), pid, coalesce(event_type,'CPU'), coalesce(event,'CPU'), queryid FROM pg_wait_sampling_history) TO STDOUT WITH CSV \watch 1
