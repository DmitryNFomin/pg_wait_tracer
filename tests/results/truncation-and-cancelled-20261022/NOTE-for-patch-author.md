# `pg_wait_event_trace_by_statement()`: two attribution gaps found in a lab run (wet-v11, tip 382f87e)

**Run:**
- gate-3 (4 vCPU AMD EPYC-Genoa, KVM), PostgreSQL 20devel built from `wet-v11`.
- `pg_wait_event_tracing` preloaded, `capture = trace`, default `trace_ring_size` (4 MB).
- Workload: 16 pgbench clients, `-M prepared`, `SELECT payload FROM t WHERE id = :id` at 8,000/s.
- Autovacuum truncated the table during the run, holding an ACCESS EXCLUSIVE lock in ~20 ms bursts.
- The rings of all 16 client backends and `pg_wait_event_trace_by_statement(procnumber)` were dumped 3 s after the
  episode.

**Data:** `tests/results/truncation-and-cancelled-20261022/raw/tw-w1/` (`rings.csv.gz`, `by_statement.csv.gz`,
`timing.csv`), harness `raw/trw.sh`.

## Gap 1: prepared statements never open a statement bucket

`by_statement()` opens a statement bucket only on `Query/QueryStart` or `Query/UtilityStart` at depth 0:

```sql
(wait_event_type = 'Query' AND wait_event IN ('QueryStart', 'UtilityStart') AND depth = 0) AS is_stmt_open
```

Executing an already-prepared statement over the extended protocol (`Bind`/`Execute` of a named statement) does not
run `post_parse_analyze`, so there is no `QueryStart`, only `ExecStart`. The C comment at `pwet_marker_exec_start()`
already says this. As a result, every wait of such an execution falls into the `<idle>` bucket opened by the preceding
`Idle` marker.

**Measured (16 backends, ~50 s of ring):**

| Bucket | Event | Calls | Total |
|---|---|---|---|
| `<idle>` | IO:DataFileRead | **27,710** | 5,089 ms |
| `<idle>` | Lock:relation | **208** | 8,088 ms |
| `<idle>` | LWLock:BufferMapping / LockManager / ProcArray, Timeout:SpinDelay | 56 | 15 ms |
| a statement | IO:DataFileRead | **1** | 0.7 ms |

This affects any client that uses named prepared statements: JDBC after `prepareThreshold`, pgx, psycopg 3 after
`prepare_threshold`, `pgbench -M prepared`, and most connection-pooled OLTP.

**Suggested fix:** also open a statement bucket on `Query/ExecStart` at depth 0 when the current bucket is `<idle>` or
`<unattributed>`, i.e. the first start marker after Idle. Take the bucket's query_id from that marker. This matches the
C state machine, where any of the three start markers moves IDLE/AFTER_STATEMENT to OPEN.

## Gap 2: waits before the statement-start marker (the "pre-statement" phase)

Some work for a statement happens after the client's message has arrived (the `ClientRead` wait has ended) but before
any start marker is written:

- **Extended protocol:** `exec_bind_message()` → `GetCachedPlan()` → `RevalidateCachedQuery()` /
  `AcquirePlannerLocks()` / `AcquireExecutorLocks()`. Relation locks are taken here, before `ExecutorStart` (the
  `ExecStart` marker). A replan also happens here.
- **Simple protocol:** parse analysis opens relations with their locks (`parserOpenTable()` → `table_openrv()`) before
  `post_parse_analyze` (the `QueryStart` marker). Catalog cache misses during parse/analysis or planning add
  `DataFileRead` here too.

Waits in that window carry `query_id = 0` on the wait record: the backend has not published the statement's query id
yet. By position they land in `<idle>`.

**Measured:** all 208 `Lock:relation` waits (avg 38.9 ms, max 101.5 ms; these are the autovacuum-truncation stalls)
have `query_id = 0`. In the raw ring, every one of them has:
- **previous** marker: `Idle` (208/208)
- **next** marker: the lookup's `ExecStart` (195) or `QueryStart` (13), query_id `-4621018123696140640`

So the information is in the ring. Only the attribution rule loses it.

**Why it matters:** this phase is where relation-level lock convoys hit: DDL/migrations queued behind or ahead of
traffic, autovacuum truncation, `LOCK TABLE`, `REINDEX`. In our run, pg_wait_sampling also could not attribute these
waits to any query: 787–821 samples with no query. A correct per-statement attribution here is something sampling
cannot provide, so it is a strong use case for the patch.

**Suggested fixes, either:**

1. **In `by_statement()`:** a `ClientRead` wait that ends opens a pending bucket. Waits after it and before the next
   start marker (`QueryStart`/`UtilityStart`/`ExecStart` at depth 0) are attributed to that next statement, marked as
   its pre-statement part (e.g. a `phase = 'pre'` column). If the next marker is `Idle`/`TxnAbort` instead (a failed
   Bind or Parse), keep them in a separate bucket, e.g. `<pre-failed>`.
2. **In C:** emit a marker when a `ClientRead` wait ends in a client backend (e.g. `Query/RequestStart`, query_id 0),
   and let `by_statement()` open a bucket on it that takes its query_id from the next start marker. That also gives a
   per-request "arrival" timestamp, useful for request latency.

**Test that would have caught both:**
- With `capture = trace`, run a prepared statement (`PREPARE` + `EXECUTE` from SQL does go through
  `post_parse_analyze`; use the extended protocol via `pgbench -M prepared` or libpq `PQexecPrepared`) while another
  session holds `ACCESS EXCLUSIVE` on the table for ~50 ms.
- Expect a `Lock:relation` wait of ~50 ms in that statement's bucket, plus the statement's own `DataFileRead` waits in
  its bucket, and none of them in `<idle>`.

## Unrelated observations from the same runs, for information

- **Cost:** with `capture = trace` the convoy run was unchanged versus `capture = off` (3,369 vs 3,052 requests > 15 ms;
  p99.99 81 vs 95 ms). pg_wait_tracer's exact tier tripled the slow requests in the same test, so this is a strong point
  for the in-core approach.
- **Ring coverage:** the default 4 MB ring held ~50 s at ~500 statements/s per backend. A 16 MB ring was used in later
  runs.
- **Ring memory under a container limit:** with `trace_ring_size = 16MB`, ~20 backends and `shared_buffers = 128MB`,
  the server running under a systemd `MemoryMax=900M` was cgroup-OOM-killed during a trace run
  (`checkout-tail-20261022`). Rings are shared memory, which cannot be reclaimed like page cache. A doc note (ring
  size × max backends) or a total cap would help.
- **Auxiliary processes:** with `capture = trace` set in postgresql.conf, `pg_stat_get_wait_event_timing()` /
  `pg_get_wait_event_trace()` returned only client backends in our dump query (`procs.csv` in the commit-convoy run).
  Check whether walwriter/checkpointer are expected to be captured.
