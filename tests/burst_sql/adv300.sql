-- b_lm: LWLock:LockManager. 300 advisory lock/unlock pairs in ONE
-- statement from each of several concurrent clients: every pair touches the
-- shared lock manager's partitioned hash table, so the partition LWLocks
-- genuinely contend. SHARED mode (not exclusive) so the clients contend on
-- the lock-manager partition rather than serialising on the advisory lock
-- itself -- the former is LWLock:LockManager, the latter would be
-- Lock:advisory, which tests/live_loop_workload.py already supplies.
SELECT pg_advisory_lock_shared(4242424242), pg_advisory_unlock_shared(4242424242) FROM generate_series(1,300);
