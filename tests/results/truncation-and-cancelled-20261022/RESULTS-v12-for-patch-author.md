# `wet-v12` re-run of the truncation convoy (tw-w1)

Thank you for the corrections. I accept all five, and the earlier note's "unchanged" cost line is replaced below.

**Run:**
- `wet-v12`, tip `ae41909b85`, on the same box, config and workload as `tw-w1`.
- Three `capture = trace` runs alternated with three `capture = off` runs.
- Data: `raw/tw-v12{t,o}{1,2,3}/`. Check script: `raw/bs12.py`.

## Attribution: as you expected, in all three trace runs

| Run | Convoy `Lock:relation` waits (stats = ring = by_statement) | avg / max | Bucket | Lookup `DataFileRead` |
|---|---|---|---|---|
| v12t1 | 144 | 45.2 / 107.1 ms | statement, `phase = 'pre'`: 144 | 34,274 in statements, 0 in `<idle>` |
| v12t2 | 208 | 38.8 / 110.1 ms | statement, `phase = 'pre'`: 208 | 27,725 in statements, 0 in `<idle>` |
| v12t3 | 32 | 94.1 / 119.2 ms | statement, `phase = 'pre'`: 32 | 27,441 in statements, 1 in `<unattributed>` |

- Only `ClientRead` remains in `<idle>`.
- **Buffered requests (v12t1):** 231 executions start right after `TxnCommit` with no `Idle`. The 22 of them with waits of their own each got their own bucket.
- **Ring dump vs `by_statement()`:** `DataFileRead` counts differ by −6 to +9 per backend. I dumped both while pgbench was still running, so the two calls saw slightly different rings. That comes from my harness, not from v12.
- **Not covered by this workload:** `<no-statement>`, `DO`/`CALL`, the simple protocol.

## Cost, four runs per side (v11 w0/w1 included)

| | Requests > 15 ms |
|---|---|
| `capture = off` | 1,436 / 3,164 / 3,530 / 3,052 (mean 2,796) |
| `capture = trace` | 2,947 / 3,086 / 1,656 / 3,369 (mean 2,765) |

- No difference is visible.
- The spread inside each configuration follows how long autovacuum holds the lock in that run (0–1.8 s), and it is much larger than any off/trace difference.
- So "no measurable cost in this test at n=4" is as far as I would go.

## Sampler, for your list example

pg_wait_sampling (10 ms, profile and 5,000-row history), all eight runs:
- The lookup's `Lock:relation` share is 0.00%.
- The history ring at the end holds no `Lock:relation` rows.
- `log_lock_waits` stayed silent.
