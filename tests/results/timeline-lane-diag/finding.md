# Timeline single-lane finding: diagnosis (no product fix)

Branch `agent/timeline-lane-diag`, base `97dca0c`. Diagnosis only, per
`brief-timeline-lane.md`.

## Verdict

**Case 1 holds: the view correctly shows one selected PID, and it IS
labelled** — both on the chart itself (the y-axis category literally reads
`PID <n>`) and in the persistent global filter chip/breadcrumb that sits
above every tab the instant a PID filter is active. This is not a defect.
**Not demo-blocking.** No issue filed for the main finding.

A second, unrelated, currently-dormant problem was found while in there and
is reported, not fixed — see "Second problem" below.

## How the single-lane shape arises (file:line)

1. **The y-axis is driven 1:1 by the payload's `pids` array**, never an
   aggregation:
   `web/static/lib/builders/timeline.js:381` —
   `const pidLabels = pids.map(p => 'PID ' + p);` — one category per element
   of `data.pids`, in payload order. There is no branch anywhere in
   `buildTimelineOption` (`web/static/lib/builders/timeline.js:370-475`) that
   merges two PIDs into one row; the per-pixel-column "aggregation" in the
   same file (`aggregateTimelineColumns`, lines 162-276) aggregates **waits
   within one PID's row** when they are too dense to paint individually
   (#106) — it never aggregates **across PIDs**, and it never changes the
   row count.

2. **A PID-filtered request can only ever return one PID, by construction at
   the data-LOAD layer**, before the handler's own logic runs:
   `src/server.c:3412-3419` (`server_load_events_fi`) →
   `server_load_events_fi_mode` → the per-record filter at
   `src/server.c:3116` (`if (pid != 0 && ce->events[i].pid != pid)
   continue;`) drops every record whose pid does not match **while loading
   from disk**, whenever `req->filter.pid != 0`. The
   handler (`handle_session_timeline`, `src/server.c:4770-4835`) never even
   sees another PID's events in that case — this is not a drop inside the
   handler and not an aggregation, it is "the server never loaded anything
   else."
3. **`req->filter.pid` is 0 — unfiltered at load — for a query_id-only
   request.** `src/server.c:594-601`: `parse_filters()` fills `filter.pid`
   only from a `pid` key; nothing else sets it. So a query_id drill-down (no
   pid) loads every PID's events and the handler's own two-pass logic
   (`src/server.c:4779-4835` first pass, `4894-4929`+ second pass) collects
   **every distinct PID that matches**, with no cap: `pending` grows via
   `realloc` doubling (`src/server.c:4816-4832`), and the `pids` JSON array
   (`src/server.c:4861-4863`) is built from that complete first pass —
   **before** any event-count truncation is applied. Event-count truncation
   (`TIMELINE_MAX_EVENTS`, coalescing) can drop *bars*, never PIDs from the
   `pids` array.
4. **Why a PID filter is the common path that produces the single lane**:
   `web/static/app.js:834` — `PIVOT = { ..., pid: 'timeline', ... }` — clicking
   any PID cell anywhere in the UI drills a `pid` filter and pivots straight
   to the Timeline tab. That is almost certainly what the UI review did:
   selected a PID, got that PID's own timeline. This is the intended,
   documented behaviour of a per-session drill-down, not a defect.

## Is the single-PID case labelled? Yes, twice over

- **On the chart**: the single y-axis category is not a generic "Lane 1" —
  it is `PID <n>` (builder code cited above), so the number is right there.
- **Above the chart, on every tab**: `web/static/index.html:119`
  (`<div id="breadcrumb"></div>`) sits above `#table-container` inside the
  persistent header (`<nav id="tabs">`...`<div id="filter-bar">` +
  `#breadcrumb`), so it renders regardless of which tab is active.
  `web/static/app.js:692` — `labels.pid = 'PID ' + restored.pid;` — the pid
  filter gets a human label (`PID <n>`), shown as a removable chip/crumb with
  its own `×`.

## Expected vs observed lane count — calculated fixture, real server

Built `pgwt-server` locally on this Mac (BPF-free; `demos/build-server-local.sh`,
confirms "nothing eBPF runs on macOS" was never in question here — this
binary has no BPF skeleton) and `tests/gen_test_traces` with the same
macOS-compatible flags. Ran a probe (not a committed test) using the existing
`tests/test_data_timeline.py::build_many_pids` generator — **a calculated
fixture, not a screenshot read**: 5 distinct PIDs
(`1000, 2024, 3048, 4072, 5096`), each issuing `query_id=100` and each
waiting twice (`IO:DataFileRead` then `Lock:relation`), so the expected
distinct-PID count is **5** and the expected bar count is **10**, both
calculated from the generator, not observed.

Evidence files in this directory:
- `repro_server_probe.py` — the probe script (reproducible: rerun
  instructions in its docstring).
- `real_server_response.json` — the **real pgwt-server's** raw
  `session_timeline` response (not `tests/mock_server.py`) for the
  `query_id=100` (no pid) request.
- `repro_server_probe_output.txt` — its stdout.
- `builder_probe.mjs` / `builder_probe_output.txt` — feeds that exact real
  response through the actual client builder
  (`web/static/lib/builders/timeline.js:buildTimelineOption`).

Results:

| | expected (calculated) | observed |
|---|---|---|
| distinct PIDs, `query_id`-only filter | 5 | 5 (`real_server_response.json: pids`) |
| bars, `query_id`-only filter | 10 | 10 |
| truncated | false | false |
| Timeline lanes the builder emits for that payload | 5 | 5 (`builder_probe_output.txt`) |
| bar→row assignment | each PID's 2 bars on its own row | rows `[0,1,2,3,4]`, 2 bars each — correct |
| distinct PIDs, same window, `pid=5096` filter | 1 | 1 (`pids: [5096]`) |

So: when the payload genuinely contains several concurrently-waiting PIDs,
the real server reports all of them and the real builder draws all of them as
separate lanes — no aggregation, no silent drop anywhere in this path. The
single lane the reviewer saw is consistent with a PID-filtered request, which
is **designed** to narrow to one PID, and that narrowing happens before the
handler runs, not as a client-side collapse.

## Second, unrelated problem found while in there (NOT fixed, reporting only)

`web/static/lib/builders/timeline.js:405` (`barData.push([... ,
pidIndexMap[ev.p] || 0, ...])`) and the matching fallback in
`aggregateTimelineColumns` (`b[2] >= 0 && b[2] < rows ? b[2] : 0`, line 186):
if an event's `p` is ever **not** present in the payload's `pids` array, the
bar is silently drawn on **row 0** (whatever PID happens to be first) instead
of being refused or dropped. Demonstrated in `footgun_probe.mjs` /
`footgun_probe_output.txt`: feeding a payload with `pids: [1001, 1002]` and
an event for `p: 9999` (not in `pids`) draws that event's bar on `PID 1001`'s
lane, with no error, no visual difference from a genuine PID-1001 wait.

Under the server code read above this is **currently unreachable**: the
handler's two passes use the identical filter, so every emitted event's pid
is already in the first pass's distinct-pid set, and the `pids` array is
never truncated independently of events. So this is dormant, not a live bug
— but it is exactly the "silently losing/misattributing lanes" shape case 3
of the brief describes, just not reachable today. Flagging it because it
would make a *future* protocol drift, a truncation change that caps `pids`
separately from events, or a hand-built fixture bug silently misattribute
waits between sessions instead of erroring loudly. Not filing an issue for
this per the brief's instruction (write it down, don't fix, don't hide); the
owner/reviewer can decide whether it is worth a defensive assertion.

## What I did NOT verify

- A real multi-backend pgbench-style capture under `--mode full` (the actual
  demo workload) rather than the synthetic `gen_test_traces` generator —
  nothing eBPF runs on macOS, so a live capture needs the gate box, which the
  brief's Gates section does not require for a diagnosis that changes no
  tracked file. The server-side mechanism (filter-at-load for `pid`,
  complete-pid-list for `query_id`) is the same code path regardless of
  whether the trace came from eBPF or the synthetic generator, so I judge
  this gap low-risk, but it is not the exact "real capture" the brief asked
  for.
- The original review's actual screenshot/session is not available in this
  worktree (no artifacts under `tests/results/` for it), so I cannot confirm
  which filter (`pid` vs `query_id`) was active when the reviewer saw the
  single lane — my reconstruction (PID-filtered drill-down) is the most
  likely path given `PIVOT.pid = 'timeline'`, but it is inference, not a
  replay of the original observation.
