# Paint-latency plan — making detail views scale

Status: **active**, opened 2026-10-08. Owner decision: ALL phases land before the
2026-10-12 demo.

## THE QUALITY BAR IS UNTOUCHABLE

Owner rule, 2026-10-08, verbatim: **"quality bar - is untouchable"**, and earlier:
*"do not relax any rules to reach this point - if you find any issues - fix it"*.

This is not a preamble, it is the binding constraint on everything below. In
concrete terms, for every phase in this document:

- **No gate is weakened.** Not the bit-exact aggregate-vs-raw cross-check (#314),
  not `make check`, not `make box-check --require-live`, not the live-UI smoke.
- **No tolerance is widened**, no retry, sleep or confirmation loop is added, and
  nothing is moved into `KNOWN_FAILING` — that table is only ever for a test
  reproducing a filed product bug being fixed in `src/`.
- **No epsilon enters the cross-check.** It is bit-exact by construction and
  stays that way.
- **A phase that cannot be pinned bit-exactly does not ship**, whatever the date.
- **A red is investigated, never accommodated.** If a gate goes red because this
  work made it stricter or found something real, that is the gate doing its job.
- **Deadline pressure changes SCOPE, never CORRECTNESS.** Each phase below has a
  minimum-shippable definition; that is the only thing permitted to flex.

If a phase cannot land before the demo without breaking one of these, it does not
land before the demo and the owner is told plainly — that outcome is strictly
preferable to a demo built on numbers that are quietly wrong, which is this
codebase's characteristic failure mode.

## The problem, in one sentence

Six detail commands load the full raw event window on **every** request, at
roughly **0.68 seconds per million records** (note the unit — a 1000x error in
this figure hid the problem for a week), so a 15-minute window costs multiple
seconds of pure loading before any computation begins.

Measured: UI paint 5504 ms against a 3000 ms bound, 6 of 11 tabs over, 2 failing
to render. The server is ~100% of paint; the client is ~0.

**Why the existing fast path does not help them.** `should_use_summaries()` routes
windows >=120 s with no pid filter onto per-second pre-aggregated records, and the
five commands that can use it answer in 12-76 ms at any window. The other six
cannot, because a per-second record cannot express *which pairs of events followed
each other*, *which executions overlapped*, or *what a session's sequence was*.
Any pid filter also forces the raw path.

**Hard ceiling, unchanged by this plan:** `load_max_events` =
`min(RAM/4, 2 GiB)/48 B` ~= 44.7M events; detail views refuse past it.

## The approach

**Precompute mergeable aggregates per immutable committed block; merge complete
blocks and decode raw only at the window's edges.**

The trace is already written as committed blocks, so the unit exists. A 15-minute
request merges N block aggregates instead of walking millions of events. This is
what Oracle (ASH plus exact aggregate counters), ClickHouse
(`AggregatingMergeTree` `-State`/`-Merge`), Lucene/LSM (immutable segments),
Parquet/ClickHouse (zone maps) and Prometheus/Thanos (hierarchical rollups) all
converged on independently.

## Non-negotiable rules for every phase

1. **Bit-exact or it does not ship.** Every phase is gated by #314's
   `tests/test_agg_raw_crosscheck.c` — no epsilon, no tolerance. This codebase's
   failure mode is not crashes, it is numbers that are quietly wrong and change
   when you resize the window.
2. **Falsifiable prediction.** Each phase states the complexity change it expects
   and is measured against it. A phase that cannot be measured cannot be claimed.
3. **Fall back, never guess.** If a block has no aggregate, or an incompatible
   version, the request recomputes from raw. Absence is never an answer.
4. **Boundary blocks decode, and selection is keyed, not uniformly half-open.**
   Only *complete* blocks inside the window merge; partial blocks at either edge
   are decoded.

   **Corrected 2026-10-09** — this rule previously read "half-open `[from, to)`
   throughout (see #316)" and that was WRONG. It conflated two different keys and
   cost a wiring round: half-opening the loader zeroed `test_data_aas` and
   `test_data_categories` ("Total AAS got 0, expected 4.0"). The correct rule:

   - **Time is half-open.** An event contributes its interval **clipped** to
     `[from, to)` (`event_window_ns`, `src/compute.c:51`).
   - **Records are selected by their key.** Trace events are selected by their
     **END** timestamp, **inclusive at both ends** (`src/server.c:2848-2850`),
     because an event ending exactly at `to` lies wholly inside the window.
     Summary seconds are selected by their **START**, so a second beginning at
     `to` is out — that is what #316 actually fixed.
   - These are the **same rule read from opposite ends of an interval**, not a
     contradiction. Selection and contribution are separate steps.
   - **Every aggregate reproduces the loader's selection AND its clip,
     bit-exact.** Reconcile the aggregate to the loader, never the loader to the
     aggregate: the loader feeds every detail command, so changing it under a
     performance banner would alter every command's answer — the #315 pattern.

   Known consequence, filed as **#328** and deliberately NOT fixed inside any
   performance phase: a record at exactly `from` is selected by both adjacent
   windows, contributing 0 ns to the later one but **count 1**, so count-based
   outputs can duplicate one record at a shared boundary. `concurrency` already
   guards this with `end_ns > from_ns`.
5. **Version is the contract.** A version records which rule the precomputed
   numbers were written under. Never ship a version whose stated contract is not
   yet true (the #315 lesson).

## Phases

### Phase 0 — precondition [DONE, #314, merged `bf65c86`]
Bit-exact aggregate-vs-raw gate, 130 checks at merge, with a coverage check
requiring every `*_from_summaries` function to be cross-checked. Everything below
depends on it.

### Phase 1 — block-level transition aggregate  [START NOW]
**New module**, standalone: per committed block, a table keyed by
`(old_event, new_event)` carrying count and summed duration, plus per-node
duration totals, plus the block's trace identity and time bounds.

- Speeds up: `transitions`, and `matrix` (which calls the same endpoint).
- Prediction: O(events in window) -> **O(blocks x distinct pairs)**.
- Why first: a pair-count table is **naturally mergeable** — merging two blocks is
  adding two tables — so it is the cheapest honest test of the whole approach, and
  it establishes the versioning, invalidation, snapshot and boundary rules every
  later phase reuses.
- Silent-wrong risk: double-counting a boundary pair, or a pair whose two events
  straddle a block edge. Pinned by a cross-check case per boundary shape.

### Phase 2 — block-level per-event and per-class totals
Extends the block aggregate with per-event counts/durations and per-class totals,
so filtered `top_events` stops forcing raw.
- Prediction: removes the raw load for class/event-filtered requests.
- Depends on Phase 1's format and merge machinery. Not parallel with it.
- **Corrected 2026-10-09 — does NOT serve `heatmap` (Histogram), at any
  wiring step.** Verified: `pgwt_compute_heatmap` (`src/compute.c:1824-1890`)
  buckets each admitted event into a **(time bucket x latency bucket)** grid.
  Phase 2's per-event table is one window-wide `hist[]` per event with no time
  dimension, so no merge depth recovers "which time bucket". The latency axis
  alone cannot be reused either: the heatmap's admission keeps `IO_WORKER`
  (no `PGWT_EVENT_FLAG_IO_WORKER` check at `compute.c:1860`, unlike the
  functions that drop it), while Phase 2's event/class admission rule drops
  it; and the heatmap selects by timestamp half-open at `to`
  (`ev_ts >= to_ns` excluded, `compute.c:1868`) while Phase 2 selection is
  inclusive at both ends (rule 4 above). Three mismatched predicates, not one
  extension — this needs its own structure. **No phase in this document
  serves `heatmap` — but that does not mean Histogram is slow.** "No index
  serves this tab" and "this tab is over budget" are different claims; a tab
  only needs an index if it is actually over budget, and a direct measurement
  (Phase 2b) shows this one is not, by a wide margin. See Phase 2b.

### Phase 2b — time-bucketed latency grid [NOT NEEDED for the paint budget]
Serving `heatmap` would need a separate mergeable structure: a per-block grid
keyed by (time bucket, latency bucket), with its own half-open-at-`to`
selection and its own admission predicate (keeping `IO_WORKER`, matching the
heatmap's existing filter, rather than Phase 2's class/event rule). This would
be a distinct phase, not an extension of Phase 2's per-event/per-class
totals — that part of the analysis stands.

**Corrected 2026-10-09 — measured, not scheduled because it is not needed.**
Direct server-side timing of `pgwt_compute_heatmap` (the raw path, confirmed
via `from_summaries=0`, not the indexed summaries fast path) against the
~30.85 min demo-rehearsal trace (span 1,851.0 s, 4,308,992 raw wait-event
samples), n=10 per window (5 reps x 2 independent process restarts):

| window (s) | median ms |
|---|---|
| 60   | 10.57  |
| 120  | 22.74  |
| 300  | 52.81  |
| 600  | 106.12 |
| 900  | 164.65 |
| 1800 | 316.82 |

Every tested window, including 1800 s (essentially the whole trace), is
comfortably under the 3,000 ms budget. At the UI's 900 s boot default,
median is 164.65 ms — roughly 18x of headroom. No breakpoint was found at
any window, and no hang occurred (the #327 probe-hang concern did not
reproduce here). Full method, raw JSON and logs:
`tests/results/histogram-window-budget/` (worktree `agent-ac8eaefc6668508c4`).

Caveats, carried verbatim because they bound what this measurement can
claim:
- **Server request latency only, not end-to-end browser paint.** JSON parse,
  compute, serialize and the pipe round-trip — not fetch, ECharts render or
  DOM layout. The real in-budget window is at most what's shown above, and
  is probably smaller once the browser side is counted.
- **Measured on this Mac, not the Linux gate box** — different silicon, I/O
  path and load, so treat this as shape-of-curve and order-of-magnitude, not
  as gate evidence. The ~18x margin at the 900 s default is what makes the
  conclusion robust to that difference.
- **This trace's density (~2,328 events/s) may be lower than a busier demo
  trace.** A denser trace costs more per window; these numbers do not
  transfer to a different, denser trace unchanged.
- The ~17,000 s (~4.7 h) figure a naive linear extrapolation from the 1800 s
  point would suggest is **extrapolation beyond the measured range, not a
  finding** — raw per-event compute need not stay linear past the tested
  range (hash-probe behavior, issue #327). It is cited here only to flag that
  it is not evidence of anything.

So this phase is **not scheduled because the tab it would serve is not
failing the budget** — naming the gap only records that no phase closes it,
not that Histogram needs closing.

### Phase 3 — executions index
Append-only execution rows (plan/exec marker boundaries) with open-execution
carry-over across blocks.
- Speeds up (row set): `executions`, `exec_scatter`, `variants`, `waterfall`.
- Prediction: the ROW SET is O(events) -> O(executions in window). **Corrected
  2026-10-09 — this does not make the whole command O(executions).**
  `executions` computes a per-row `n_events` by definition (accrued per wait
  event while a row is the attributable top of its pid's open-execution stack,
  `src/compute.c:4061`), so its Leader-events/Workers columns stay
  O(window events) regardless of how the row boundaries are found; no
  boundary index changes that. What IS fully index-serveable is
  `exec_scatter`, because its output — `t`, `duration_ms`, `pid`, `query_id`,
  `in_progress` (`src/server.c:4859-4867`) — carries none of that per-row
  event-carry state.
- **Corrected 2026-10-09 — Phase 3's SERVE path is CANCELLED, not deferred.**
  A seeded-prefix replay cannot be bit-exact: `pgwt_compute_executions` carries
  a fourth cross-edge quantity beyond the three originally identified,
  `top_attributable` (set at `src/compute.c:3796`, cleared at `:3806`) — a pop
  clears it even when another row remains on the pid's open stack, so seeding
  only the open rows at a boundary can credit a window's waits to whichever
  execution replay leaves on top, which is not always what a full raw replay
  from trace start credits. Phase 3 lands as **correctness guards only**
  (cross-check coverage for the executions path), not as a serving fast path.
  The wiring described for `executions`/`exec_scatter`/`waterfall`/`variants`
  elsewhere in this document's "minimum shippable" section predates this
  correction and is not reconciled here.
- Largest phase; needs its own format decision. Independent of Phase 2.

### Phase 4 — concurrency interval index
Per-pid wait intervals with onset ordering, for `concurrency` — the one view that
needs distinct simultaneous pids, which per-second totals cannot recover.

**Why an interval list is the only mergeable structure here:** peak is
`max over (bucket, event) of |distinct pids|`, and **sets union, they do not add**
— so a per-block "peak per bucket" can never be merged. One pid waiting across
blocks N and N+1 in the same bucket is one pid, not two.

**The boundary rule that makes it safe:** store each interval ONCE, in the block
where its record lives (its END — `concurrency` already qualifies by
`timestamp_ns` falling in-window), with its true `start_ns` **unclipped**. A wait
starting in block N and ending in N+1 then sits in N+1's list and overlaps N's
buckets exactly as the raw path does today.

**Honest size of the win:** this is a *compaction* — waits-only records instead
of 48-byte everything, and no filter pass — NOT the O(blocks) jump Phases 1 and
2 get. **Corrected 2026-10-09** — this previously estimated 16-byte records at
roughly 3-6x; measured, rows are **24 bytes**, giving **6.0x** compaction. Do
not claim otherwise.

**Three silent-wrong modes, all of which under-report rather than error:**
1. **Clipping `start_ns`** to the block start -> lower peaks in earlier buckets
   and onsets shifted later, so bursts silently vanish.
2. **An interval counted twice** (once from a whole block's index, once from an
   edge decode) -> peaks survive via pid dedup, but the per-pid burst counter
   double-counts one pid toward the threshold.
3. **Silently "fixing" the raw path's existing exclusion** of waits still open at
   `to` — a semantic change hidden inside a performance phase.

**Test shape that pins it:** synthetic trace carrying every boundary class —
start before window/end inside; start in block N/end in N+1; same pid in two
blocks in the same bucket; a wait open past `to`; and a >=threshold burst whose
onsets straddle a block boundary within 10 ms — asserting `peak_sessions[]`,
`peak_event[]`, every burst's pid list and `bursts_total` byte-equal, at two
bucket resolutions. **Comparing peaks alone is the mistake to refuse.**

- Independent of Phases 2 and 3 in design; queued behind them in practice by the
  three-agent cap and by the serialised wiring queue. Its honest status is
  **designed, built if the queue clears** — it is last in the merge queue and
  therefore least likely to make the cutoff.

## Delivery: ALL phases land before the demo (2026-10-12)

Owner decision 2026-10-08: all four phases ship before the demo. This section is
the critical path and the scope control that makes that achievable **without
weakening a single gate** — the owner's standing rule is that no rule is relaxed
to reach a date. What flexes is scope *inside* a phase, never the bit-exact gate,
never `box-check`, never a tolerance.

### Critical path (the only genuinely serial chain)

```
re-cut (#316/#317/#318) ──> Phase 1 wiring ──> Phase 2
        holds compute.c,         extends P1's format
        server.c, crosscheck     and merge machinery
```

Everything else runs in parallel from now:

| | can start | blocked by |
|---|---|---|
| Phase 1 module (standalone) | **now** | nothing |
| Phase 3 executions index (standalone) | **now** | nothing |
| Phase 4 concurrency interval index | **designed now, built when a slot frees** | the 3-agent cap, and P1's format rules |
| Phase 1 wiring into `transitions` | when re-cut lands | re-cut |
| Phase 2 | when Phase 1's format is fixed | Phase 1 |

**So the binding constraint is the re-cut landing.** It is committed and rebasing;
landing it is therefore the highest-priority action, ahead of any new work.

Each phase splits into a **standalone module** (new files, no contention, exact
unit tests) and a **wiring step** (touches `src/compute.c` / `src/server.c` /
`tests/test_agg_raw_crosscheck.c`, which are contended and must be serialised).
Modules are built in parallel; wiring is queued. `tests/unit_tests.list` has a
single writer — agents report their line, the orchestrator applies it.

### Minimum shippable per phase

**Owner requirement 2026-10-08, verbatim: "i need all tabs!"** — so NO TAB may be
left on the slow path. Scope flexes only *within* a tab's capability (a filtered
request may still fall back to raw); it never flexes by dropping a tab. An earlier
draft of this section deferred `variants` and `exec_scatter` to "after"; that is
withdrawn, because it meant two tabs staying slow.

All eleven tabs must be inside the paint budget. **Corrected 2026-10-09 —
Histogram (`heatmap`) already is, measured, without any phase serving it**
(see Phase 2b's measurement); no index is scheduled for it because none is
needed. The genuinely over-budget tabs are served by:

| phase | tabs it must bring inside budget |
|---|---|
| Phase 1 | `transitions`, `matrix` |
| Phase 2 | filtered `top_events` (Events) |
| Phase 3 | `exec_scatter` (row set only; see 3b for `variants`) |
| Phase 3b | `variants` — needs a per-execution STEP-SEQUENCE index, not boundaries |
| Phase 4 | `concurrency` |
| — | `waterfall`, `executions` — no phase currently serves these (Phase 3's cancellation note); unlike `heatmap`, these are not independently confirmed in-budget, so they stay listed as over-budget until measured or served |

What may still flex, per phase, without leaving a tab behind:

- **Phase 1:** unfiltered `transitions` merges block aggregates; class/event-
  filtered requests may fall back to raw in the first cut. Both tabs are served.
- **Phase 2:** per-event totals before per-class totals; filtered `top_events`
  served either way. Does not touch Histogram — see this phase's
  2026-10-09 correction above.
- **Phase 3:** row set only, and only for `exec_scatter` — see this phase's
  2026-10-09 cancellation note above. `waterfall` and `executions` are not
  served by it.
- **Phase 3b:** `variants`. **Correction made 2026-10-09 by the Phase 3
  implementer, and it is a correction to this plan, not a shortfall in the work:**
  a boundary index cannot serve `variants`. `handle_variants` builds flow patterns
  from the event SEQUENCE between markers (`pgwt_compute_variants` over raw
  events), so execution boundaries alone cannot produce the answer. It needs a
  separate per-execution step-sequence index with partial-sequence carry-over
  across blocks. Phase 3 serves three of its four named commands fully; `variants`
  is its own slice. Under the all-tabs requirement this slice is REQUIRED, not
  optional.

  Also from the same work, carried forward as wiring requirements rather than
  defects: `n_events` / `n_workers` / `matches_event_filter` are window-clipped and
  filter-dependent, so the index poisons them to -1 with `counts_indexed = 0`
  rather than zeroing (a zero would read as a true count) — therefore a
  class/event-filtered `executions` or `exec_scatter` still needs raw. And index
  coverage is *containment*, not intersection: a window reaching past the last
  committed block REFUSES rather than silently shortening itself, so the common
  "ends at now" case always needs a raw tail decode against the index's public
  `cover_to_ns`.
- **Phase 4:** interval index serving BOTH peak and bursts, unfiltered only;
  filtered requests fall back to raw. The earlier "intervals without onset
  ordering" split is withdrawn as incoherent: the index stored sorted by
  `start_ns` **is** the onset ordering, and bursts are then one sliding pass over
  the merged sorted lists. That split would have shipped the hard part and
  withheld the free part.

Every one of those is still gated bit-exactly. A phase that cannot be pinned does
not ship, whatever the date.

### The real constraint: serialised gate time, and the merge cutoff

Implementation is not the binding constraint. **Four wiring steps are**, each
touching `src/compute.c` / `src/server.c` / `tests/test_agg_raw_crosscheck.c`,
which cannot be parallelised. Each wiring step costs, serially:

```
box-check (~40 min)  +  review round  +  CI  +  merge
```

That is most of a day of pure gate time for four phases, before a single
rehearsal runs. Three agents can implement in parallel; they cannot merge in
parallel.

**Merge cutoff: the last performance merge lands by the evening of 2026-10-10.**

That date is not a preference, it is arithmetic: two clean rehearsals must follow
the last merge, a red rehearsal resets the clean-attempt counter, and the GIF is
cut after the rehearsals. Anything not merged by then does not go in the demo —
not because the work is abandoned, but because shipping it later means showing a
tag nobody rehearsed.

Writing the cutoff down is what turns "all phases land before the demo" from an
instruction into a **testable claim**. If a phase misses the cutoff, that is a
measurable fact to report, not a judgement call to argue about.

### Named risks

1. **Phase 2 is serial behind Phase 1 and cannot be parallelised** — it extends
   the same on-disk format. If Phase 1's format needs a second iteration, Phase 2
   absorbs the delay. Mitigation: Phase 1 fixes its format early and publishes it
   before the wiring is finished.
2. **Three agents is the honest implementation cap**, and a fourth queues rather
   than speeds anything up. Two concurrent `make check` runs is the Mac's limit.
3. **A silently wrong aggregate is worse than a slow one.** Every phase answers
   its adversarial question ("what input makes this pass while being wrong?") in
   writing before it ships.
4. **Rehearsals still have to happen after this lands**, and a red resets the
   clean-attempt counter. Landing all four phases at the last moment leaves no
   rehearsal room — so phases merge as they finish, not in one batch at the end.

## Parallelism

Phase 1 blocks on the cross-check re-cut branch (`#316`/`#317`/`#318`), which
holds `src/compute.c`, `src/server.c` and `tests/test_agg_raw_crosscheck.c`.
Phase 1's **new module** can be built and unit-tested standalone immediately; the
wiring into those three files waits for that branch to land. Phases 3 and 4 are
mutually independent once Phase 1's format rules exist.
