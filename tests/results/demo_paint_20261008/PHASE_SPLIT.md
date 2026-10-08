# Where the paint time actually goes — measured, 2026-10-08

Two hypotheses died because both were inferred from end-to-end latency
(the coordinator's event volume, my session churn). This is the localising
measurement. **The answer is unambiguous: the server is ~100 % of it, and the
dividing line is the summary fast path versus the raw path.**

## Method

`pgwt-server` driven directly over its stdin/stdout JSON protocol — **no
bridge, no ssh, no browser, no instrumentation**, so the number is server wall
time per command and nothing else. One command per request, serial, against the
**retained r2 capture** (the same trace the 5504 ms walk measured), last 900 s
window = the UI's own range.

    trace       /root/paint1_trace_r2 on gate-1 (pgwt-gate)
    num_events  6,955,008 whole trace; window 900.0 s
    binary      pgwt-server built from this branch at 766120a
    driver      tests/results/demo_paint_20261008/timecmds.py

## Result

| command | server ms | the tab it feeds | that tab's measured paint (r2) | client share |
|---|---|---|---|---|
| `top_queries` | **5717** | queries | 5504 | **~0** |
| `variants` | **5469** | transitions (un-awaited 2nd request) | — | — |
| `executions` | **4989** | waterfall | 4991 | **~0** |
| `exec_scatter` | **4690** | scatter | 4405 | **~0** |
| `concurrency` | **4141** | concurrency | 3488 | **~0** |
| `transitions` | **3671** | transitions / matrix | 3023 | **~0** |
| `time_model` | 52.7 | overview | 315 | ~260 |
| `heatmap` | 57.2 | histogram | 360 | ~300 |
| `top_sessions` | 50.4 | sessions | 313 | ~260 |
| `top_events` | 47.8 | events | 317 | ~270 |
| `aas` | 47.4 | (every tab's AAS strip) | — | — |

## What this settles

1. **The client is not the problem.** For all six heavy tabs the server's own
   time equals or exceeds the whole click-to-painted figure, so the client
   share is zero within measurement error. The `+~200 ms render` term in the
   old model is still about right — it is visible as the 260-300 ms residual on
   the five light tabs, and it is immaterial against 3.7-5.7 s. **Every
   client-side lever is the wrong end**, and the harness concern about
   measuring to `mount()` instead of the next painted frame cannot change a
   conclusion where the client term is ~0. I have therefore NOT rebuilt the
   harness's frame accounting; if the server work lands and tabs come under
   ~1 s, that term starts to matter and should be fixed then.

2. **The split is binary: summary path ~50 ms, raw path 3.7-5.7 s.** Nothing
   sits in between. The five fast tabs are the five that answer from summary
   files at any window size; the six slow ones are the six that load raw
   events.

3. **The dominant term is the SHARED raw load, not per-tab aggregation.**
   `transitions` is the cheapest raw-path command at 3671 ms and it walks the
   window once, so the common load is **≤ 3.7 s** and each tab's own
   aggregation adds 0-2 s on top (`top_queries` +2046 ms, `concurrency`
   +470 ms, `executions` +1318 ms). Consequence worth stating plainly: a
   perfect fix to every aggregation still leaves ~3.7 s, which is **over the
   3000 ms bound on its own**. Any fix that closes the gap has to make the
   raw load cheaper or avoid it.

4. **`variants` is 5469 ms, not 14-16 s.** This **contradicts** the arithmetic
   the second adviser's Priority 1 rested on. A single orphaned `variants`
   queued ahead of `concurrency` is 5469 + 4141 = 9.6 s, comfortably inside
   the harness's 15 s deadline, so one orphan does **not** explain a
   non-render. Two orphans would: `transitions` 3671 + `variants` 5469 +
   `concurrency` 4141 = 13.3 s, plus the reload's own `info`/`aas`/`time_model`
   (~150 ms) and overview's `time_model`, which crosses 15 s. So the serial
   orphan mechanism stays the leading explanation for the render failures, but
   it requires orphan ACCUMULATION rather than one long command, and that is
   **not yet demonstrated** — it needs the request-id/mount-sequence evidence
   from a live failed navigation.

## The specific waste this exposes on the worst tab

`src/server.c`'s `handle_top_queries` loads the full raw window
**unconditionally**, before the branch that routes to the summary fast path:

    all_events = server_load_events_fi(srv, req->from_ns, req->to_ns, 0, ...);
    if (reject_overload(...)) return;
    if (!has_event_filter && should_use_summaries(srv, req, &wfid)) {
        pgwt_compute_top_queries_from_summaries(...);   /* the ~50 ms path */
    } else {
        pgwt_compute_top_queries(all_events, ecount, ...);
    }
    ...
    pgwt_compute_query_lifecycle(all_events, ecount, &lcres);  /* needs markers */

Its own comment says so: *"Always load raw events for lifecycle stats."* So the
window's class breakdown is computed in ~50 ms and then the tab pays ~5.7 s for
a raw load whose only remaining consumer wants **markers**.
`server_load_markers_fi` (`src/server.c`, the `markers_only=1` wrapper)
already exists.

**Not yet justified as a fix**, and deliberately not made: the markers-only
path still iterates every event on the cached path, so the gain depends on
whether the 5.7 s is the walk or the materialise-and-rescan. That is precisely
the load-versus-compute split, which `PGWT_PHASE_TIMING=1` (added to
`handle_top_queries` in this branch) answers in one run. It needs ~2 minutes of
gate-1 CPU and was blocked at the time of writing: `box-check`'s live-UI-smoke
walk was in flight and is exactly the timing-sensitive tier I must not perturb.

## Instrumentation added

`src/server.c`: `phase_now_ms()`, `phase_timing_on()`, `PHASE_MARK()` and one
`fprintf` in `handle_top_queries`, all behind `PGWT_PHASE_TIMING` (unset or
"0" = off, which is a single cached branch). Output goes to **stderr**, never
stdout, because stdout carries the newline-delimited JSON the bridge parses and
a stray line there would corrupt a response rather than annotate it.
