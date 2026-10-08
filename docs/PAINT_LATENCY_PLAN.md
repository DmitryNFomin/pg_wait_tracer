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
4. **Boundary blocks decode.** Only *complete* blocks inside the window merge;
   partial blocks at either edge are decoded. Half-open `[from, to)` throughout
   (see #316).
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
so `heatmap` and filtered `top_events` stop forcing raw.
- Prediction: removes the raw load for class/event-filtered requests.
- Depends on Phase 1's format and merge machinery. Not parallel with it.

### Phase 3 — executions index
Append-only execution rows (plan/exec marker boundaries) with open-execution
carry-over across blocks.
- Speeds up: `executions`, `exec_scatter`, `variants`, `waterfall`.
- Prediction: O(events) -> O(executions in window).
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

**Honest size of the win:** this is a *compaction* — 16-byte waits-only records
instead of 48-byte everything, and no filter pass — so roughly **3-6x**, NOT the
O(blocks) jump Phases 1 and 2 get. Do not claim otherwise.

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

All eleven tabs must be inside the paint budget. The six currently over it are
served by:

| phase | tabs it must bring inside budget |
|---|---|
| Phase 1 | `transitions`, `matrix` |
| Phase 2 | `heatmap` (Histogram), filtered `top_events` (Events) |
| Phase 3 | `waterfall`, `executions`, `variants`, `exec_scatter` |
| Phase 4 | `concurrency` |

What may still flex, per phase, without leaving a tab behind:

- **Phase 1:** unfiltered `transitions` merges block aggregates; class/event-
  filtered requests may fall back to raw in the first cut. Both tabs are served.
- **Phase 2:** per-event totals before per-class totals. Histogram served either
  way.
- **Phase 3:** completed executions are indexed; an execution open at a window
  edge may fall back to raw rather than be guessed. All four tabs served, because
  the common case is indexed.
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
