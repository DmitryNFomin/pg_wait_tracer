# CORRECTION to PHASE_SPLIT.md, and the measured window curve

**Two claims in `PHASE_SPLIT.md` were wrong, both from a bug in my own driver.**
`src/server.c`'s `parse_request` reads `cJSON_GetObjectItem(root, "from")` and
`"to"`. My first `timecmds.py` sent `from_ns` / `to_ns`, which the server
silently ignored, so `req->from_ns = req->to_ns = 0` and every request
defaulted to `srv->earliest_wall_ns .. srv->latest_wall_ns` — **the whole
1110 s trace, 6.96M events, not the 900 s / 5.70M window the UI asks for.**
The times were real server times for the wrong window.

### Retracted

1. ~~"client share ~0 on all six heavy tabs"~~ — **wrong.** It came from
   comparing a whole-trace server time against a 900 s paint. See the table
   below: the server is **61-81 %** of paint, and the client+transport share is
   ~270-320 ms on the light tabs rising to **~1.0 s** on the largest payloads.
   **Consequence: I over-retired the "measure to the next painted frame rather
   than `mount()`" item.** At a ~1 s client share on `queries` and `scatter`
   that term is no longer immaterial, and the harness's accounting should be
   fixed before anyone attributes the last second.
2. ~~"`handle_top_queries` loads the whole trace regardless of window"~~ —
   **wrong, and purely my bug.** With the right field names the window is
   honoured exactly: 5,699,738 of 6,955,008 events = 82 % = 900/1110.

### Unchanged by the correction

The server is still the dominant term, the summary/raw split is still binary
(~40-49 ms vs 2.7-4.5 s), and aggregation is still negligible on the summary
path. Every strategic conclusion holds; two numbers inside it did not.

## Corrected: 900 s window, 5,699,738 in-window events

| command | server ms | tab | r2 paint | client+transport |
|---|---|---|---|---|
| `top_queries` | 4491 | queries | 5504 | ~1013 |
| `executions` | 4217 | waterfall | 4991 | ~774 |
| `variants` | 3621 | transitions (2nd, un-awaited) | — | — |
| `exec_scatter` | 3470 | scatter | 4405 | ~935 |
| `concurrency` | 3074 | concurrency | 3488 | ~414 |
| `transitions` | 2710 | transitions / matrix | 3023 | ~313 |
| `time_model` 40.2 · `aas` 46.1 · `top_sessions` 46.3 · `top_events` 47.0 · `heatmap` 48.5 | | the 5 light tabs | 313-360 | ~267-312 |

## The phase split, corrected

    PHASE top_queries load=2602.4 compute=108.6 serialise=1778.4
                      total=4489.4 events=5699738 from_summaries=1

- **load 2602 ms (58 %)** — ~460 ns/event over 5.70M events.
- **compute 109 ms (2 %)** — the summary path, as expected. Aggregation is a
  rounding error; the "doubled passes" family cannot matter.
- **1778 ms (40 %)** between the lifecycle compute and `emit_json` returning.
  My label "serialise" is **imprecise** — that span also contains the per-row
  context/attribution work, not just the JSON build, and 1.78 s for **26 rows**
  cannot be JSON encoding. It needs one more split before anyone optimises it.

`transitions` walks the same window once and totals 2710 ms ≈ the load alone,
which independently confirms **the shared raw load is ~2.6-2.7 s.**

## The owner's `5m` lever, now MEASURED (it was n=0)

Server ms by window; add the per-tab client share above for a paint estimate.

| command | 120 s | 300 s (`5m`) | 600 s | 900 s |
|---|---|---|---|---|
| `top_queries` | 707 | 1708 | 2727 | 4415 |
| `executions` | **2097** | **2633** | 3132 | 4184 |
| `exec_scatter` | 1611 | 2010 | 2602 | 3619 |
| `concurrency` | 468 | 1205 | 2035 | 3338 |
| `transitions` | 435 | 1031 | 1736 | 2944 |
| `variants` | 558 | 1300 | 2038 | 3547 |
| `time_model` | 20.5 | 29.2 | 32.7 | 43.0 |

**At 300 s, five of the six heavy tabs come under 3000 ms once the client
share is added — but `executions` (waterfall) lands at ~2633 + 774 ≈ 3.4 s and
stays OVER.** So the zero-code fallback does not by itself clear the bound.
That is a measured correction to a lever that was previously a prediction.

**`executions` has a ~1.8 s window-INDEPENDENT fixed cost**: 2097 ms at 120 s
against 4184 ms at 900 s — 2.0x for a 7.5x window, while every other command
scales roughly linearly. That is the largest single fixed cost in the system
and it is consistent with the adviser's `load_execution_rows` 260 MB memcpy
note. It is the one target that would help at *every* window size.
