# Reply to NOTE-for-patch-author.md

Thank you for this note. It is careful work, and the main finding is correct: `pg_wait_event_trace_by_statement()`
in v11 loses exactly the waits that are most interesting for lock convoys. I checked every claim against the v11
source (tip 382f87e) and against `raw/tw-w1/rings.csv.gz`. Below is what I confirmed, three places where the
evidence or the proposed fix needs a correction, and what v12 will do.

## Confirmed

- **Gap 1 is a bug.** The SQL function opens a statement bucket only on `QueryStart`/`UtilityStart` at depth 0.
  The C marker state machine already treats `ExecStart` as a start, so the SQL and the C disagree. Every execution
  of a named prepared statement lands in `<idle>`. Your by-statement totals match the dump exactly.
- **Gap 2 is real.** In the bind path, `GetCachedPlan()` takes the relation locks before `PortalStart()`, which is
  where `ExecStart` is written. In the simple protocol, parse analysis opens the relations before
  `post_parse_analyze`. In the ring all 208 `Lock:relation` waits come after `Idle` and before `ExecStart` (195) or
  `QueryStart` (13). Average 38.9 ms, maximum 101.5 ms, as you wrote.
- **The dump is complete:** 16 rings, no gaps in the sequence numbers.

## Corrections

1. **The `query_id = 0` evidence does not show anything.** In v11 only marker records carry a query id. Wait
   records always report 0. In this dump all 441,808 wait records have `query_id = 0`, not only the 208 lock
   waits. Also, `exec_bind_message()` publishes the query id before it calls `GetCachedPlan()`, so "the backend
   has not published the query id yet" is not the reason. The conclusion is still right, but it rests on the
   position of the waits in the ring, not on their query id.

2. **The proposed fix for Gap 1 misses some executions.** Opening a bucket on `ExecStart` only when the current
   bucket is `<idle>` is not enough. In this dump, 266 executions start directly after `TxnCommit`, with no `Idle`
   between them. This happens when the next request is already in the socket buffer: the backend never waits in
   `ClientRead`, so no `Idle` marker is written. Pipelined executions look the same. With your rule these
   executions would merge into the previous statement's bucket. There is a second trap: inside a top-level `DO` or
   `CALL`, statements also write markers at depth 0, so a plain "`ExecStart` at depth 0 opens a bucket" rule
   would make one bucket per loop iteration.

3. **Both fixes for Gap 2 depend on a `ClientRead` wait.** The same 266 cases have no such wait. The boundary
   has to come from the last `Idle`, `TxnCommit` or `TxnAbort` marker instead.

4. **Auxiliary processes.** Your `procs.csv` in the commit-convoy run came from `pg_stat_get_wait_event_timing()`,
   and it does list walwriter, checkpointer, bgwriter, the io workers and the autovacuum launcher. Only their
   trace rings were empty. That is the documented behaviour: a server-side process starts tracing at the first
   configuration reload that sets `capture = trace`, not at postmaster start, because a ring for every such
   process would be reserved even when nobody traces. For the commit-convoy test, where walwriter matters most,
   please run `SELECT pg_reload_conf();` once after the server starts.

5. **The cost line.** 3,369 against 3,052 requests over 15 ms is about 10% more slow requests in one of the two
   configurations, from one run of each. I would not call it "unchanged", and I would not call it a regression
   either. It needs repetitions before anyone quotes it.

## What v12 does

The fix is in the SQL function only. The hooks and the C code do not change.

- `ExecStart` at the top level opens a statement bucket, unless a `QueryStart` or `UtilityStart` for the same
  statement already opened it (simple query, plan revalidation in Bind, pipelined Parse).
- Statements run inside a top-level `DO`, `CALL`, `EXECUTE` or `CREATE TABLE AS` stay in that statement's bucket.
- Waits between the last `Idle`, `TxnCommit` or `TxnAbort` and the statement's start marker go to that statement,
  marked with a new column `phase = 'pre'`. `ClientRead` itself stays in `<idle>`, because it is idle time.
- Waits of a request that fails before any statement starts (for example `lock_timeout` during Bind) go to a new
  `<no-statement>` bucket.
- New tests cover prepared statements over the extended protocol, a pipeline with a sync between executions, a
  `DO` loop, and a lock held by a second session during Bind and during parse analysis.

Two cases stay ambiguous, and the documentation will say so. In an explicit transaction or a pipeline with no
commit between statements, waits between one statement's end and the next one's start stay with the earlier
statement. When the next request was already buffered, end-of-transaction work after `TxnCommit` counts as the
next statement's `pre` phase.

The work is on branch `wet-v12` of the fork. When it passes CI I will tell you, and it would help a lot if you
could repeat the `tw-w1` run on it. The result I expect is the 208 lock waits in statement buckets with
`phase = 'pre'`, and almost no `DataFileRead` left in `<idle>`.

Thanks again. The point that sampling could not attribute these lock waits to any query is a strong use case for
the patch, and I would like to use this example on the list once v12 is out.
