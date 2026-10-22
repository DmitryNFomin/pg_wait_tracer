# #486 — scoped-request cross-check (fix/answer-contracts)

Not a test, not a gate — a saved, re-runnable measurement. Produced while
implementing branch A of the answer-contracts plan, per the owner's #486
comment: "[confirm] with a single scoped request rather than assumed"
whether the Queries tab's `exec_count` and a query_id-scoped `executions`
request agree.

Run it with: `python3 tests/results/issue486-measurement/probe.py` (needs
`pgwt-server` + `tests/gen_test_traces` built). `sample-run.txt` is the
captured stdout of one run; `results.json` is machine-readable.

## Result

| quantity | value |
|---|---|
| ground truth (EXEC_START/END pairs authored for the query id) | 15 |
| `executions{filters:{query_id:777}}.total_count` | 15 |
| `executions{...}.truncated` | false |
| `top_queries` row for that query id, `exec_count` | 15 |
| all three agree | **yes** |

## Conclusion

Confirms the owner's corrected #486 diagnosis: `src/server.c`'s execution
count is honest. `execution_matches_request` (`:5410`) filters by
`query_id` exactly, and `handle_executions` computes `total_count` from the
filtered set *before* the `limit` truncation (`:5557`-`:5610`) — the same
accounting `handle_top_queries`' lifecycle pass uses for `exec_count`
(`src/compute.c:5324`, counted across the same `from`/`to` window). A
window-wide count sitting above a two-row scroll pane (the live footage
in #486) is therefore a **client-side disclosure gap**: the Waterfall tab's
`requests()` (`web/static/views/waterfall.js:284`) sends
`filters: ctx.filters.snapshot()` with no `query_id`, so a query drill is a
*selection intent*, not a server-side filter — the 100-row response really
is the window-wide top-100 by duration, not a filtered, mis-scoped one. No
server-side divergence found. No file under `src/` was changed for #486.

## Known gap in this probe (noted in review)

This fixture's 15 executions stay under the `executions` command's default
limit (100), so `truncated` is `false` throughout — this probe never
exercises the `truncated=true` / "more than `limit`" case that the client's
disclosure guard (`web/static/views/waterfall.js:383`,
`model.waterfall.truncated && model.waterfall.total_count != null`) keys
on. The conclusion above survives on code reading (the same
filter-before-truncate order applies regardless of row count), but whichever
branch implements the #486 client disclosure (branch E) should add a
`>limit` case to its own gate.
