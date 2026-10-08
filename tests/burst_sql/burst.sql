-- b_wal: the commit path. One tiny INSERT per transaction at a high
-- transaction rate, so nearly all of the time is WAL insert + WAL write +
-- fsync -- LWLock:WALWrite / LWLock:WALInsert / IO:WALSync, which is what a
-- real OLTP system's wait profile is dominated by.
INSERT INTO pgwt_burst(ts, pad) VALUES (clock_timestamp(), repeat('x', 200));
