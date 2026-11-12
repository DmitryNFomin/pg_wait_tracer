# Contract: wait_time and completed_waits — version 1

Scope: ONE backend generation, ONE wait event, an arbitrary half-open window, EXACT evidence only
(no sampling, no summaries, no filters by query or category).

What these numbers are, and are not: the time one backend process spent reporting one named wait
event, and how many waits of that event it finished. They are not a session's total waiting (other
events are excluded), not query latency, not CPU time, and not a logical connection that survives a
backend restart. A label shown to a user must say "time in <event>" for a backend, not "session
wait time".

## 1. Vocabulary

- Times are unsigned 64-bit integers, nanoseconds, one monotonic clock: `0 <= t <= 2^64-1`. All
  arithmetic is exact integer arithmetic; no floating point and no signed 64-bit accumulators.
- **Backend generation**: one PostgreSQL backend process from start to exit. A reused process id is
  a different generation.
- **Lifetime** `L = [life_start, life_end)`, `life_start <= life_end`. `life_end = null` means the
  backend has not been seen to exit; treat as unbounded.
- **Wait interval** `[event, start, end]`, `start <= end`: the backend reported wait `event` from
  `start` (inclusive) to `end` (exclusive); `end` is the instant the wait closed. Its **occupied
  time** is `[start, end)`. `start == end` is a legal **zero-length wait**: no occupied time, but a
  real close at that instant.
- **Open wait** `[event, start, as_of]`, `start <= as_of`: at instant `as_of` the backend was
  observed still in wait `event`, entered at `start`; no close is recorded. Its occupied time is
  `[start, as_of)` — an observation at `as_of` says nothing about the nanosecond after it. At most
  one per generation. It is never extended and a close is never invented for it.
- **Coverage**: a list of spans `[a, b)`, `a <= b`, sorted by `a`, whose positive-length interiors
  do not overlap (they may touch). A span certifies COMPLETENESS, two things at once:
  (i) state: at every instant of `[a, b)` the evidence contains the wait the backend was in, if
      any; an instant in the span that lies in no occupied time was truly "not waiting";
  (ii) closes: every wait whose close instant `t` satisfies `a <= t < b` is in the evidence,
      including zero-length waits; and if `b == life_end` (the span runs up to the exit), every
      wait that closed exactly at the exit is in the evidence too.
  Coverage says the evidence is complete there. It does not say records outside coverage are
  false: a wait recorded outside coverage is still a real observation, merely not known to be the
  whole story.
- `event` is a non-empty string compared exactly (no case folding, no registry).
- The order of the `waits` list carries no meaning. Identical records are distinct records.

## 2. Valid evidence

Evidence is valid when ALL hold; otherwise the result is `evidence_invalid` (section 5):
 1. every time is an integer in `[0, 2^64-1]`; every list has the stated shape; `event` non-empty;
 2. `life_start <= life_end` (when not null);
 3. every wait has `start <= end` and lies in the lifetime: `life_start <= start` and
    `end <= life_end` (a wait may close exactly at the exit);
 4. occupied times of different waits do not overlap (they may touch);
 5. a zero-length wait does not lie strictly inside another wait's occupied time
    (`start < t < end`); at the boundary of another wait it is allowed;
 6. open wait: `start <= as_of`, `life_start <= start`, `as_of < life_end` when the exit is known;
    no closed wait has `end > open.start`, and no zero-length wait lies after `open.start`;
 7. coverage spans: `a <= b`, sorted by `a`, positive interiors disjoint, each inside the lifetime
    (`life_start <= a`, `b <= life_end`);
 8. when an open wait exists, no coverage span extends beyond `as_of` (`b <= as_of`): nothing is
    known after the last observation of a wait that never closed.

## 3. Query

`{event, from, to}`. The window is `W = [from, to)`.
- `from >= to` (including reversed windows) is `invalid_window`.
- a missing field, a non-integer or out-of-range time, or an empty `event` is `invalid_query`.

## 4. Definitions

Let `R = W ∩ L`: the part of the window in which this generation existed. A generation has no
occupied time outside its lifetime, so nothing outside `L` needs coverage.

**Required evidence for wait_time:** every instant of `R` is covered.

**Required evidence for completed_waits:** every instant of `R` is covered, AND, if the exit lies
in the window (`from <= life_end < to`), a coverage span ends exactly at `life_end` (so closes at
the exit are certified). 

If the required evidence is not there, the metric is unavailable with `coverage_incomplete`.

**wait_time(event, W)** = `closed_ns + open_ns`, where
- `closed_ns` = sum over wait intervals of that event of `max(0, min(end, to) - max(start, from))`;
- `open_ns` = the same expression for `[start, as_of)` of the open wait if its event matches,
  else 0.
A wait that closes after `to` contributes only its part inside `W`.

**completed_waits(event, W)** = the number of wait intervals of that event with `from <= end < to`.
The open wait is never counted. Zero-length waits are counted. The count does not depend on
whether the interval contributes any time to `W`, nor on `R`: a close exactly at the exit counts
when the exit instant is in the window.

## 5. Result object

```
complete:
{"metric":"wait_time","version":1,"value":4,"unit":"ns","basis":"observed",
 "completeness":"complete","reasons":[],
 "detail":{"closed_ns":4,"open_ns":0},
 "diagnostic":{"recorded_ns":4,"covered_ns":6,"required_ns":6}}
{"metric":"completed_waits","version":1,"value":1,"unit":"count","basis":"observed",
 "completeness":"complete","reasons":[],
 "diagnostic":{"recorded_count":1,"covered_ns":6,"required_ns":6}}

unavailable, evidence usable:
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed",
 "completeness":"unavailable","reasons":["coverage_incomplete"],
 "diagnostic":{"recorded_ns":4,"covered_ns":10,"required_ns":15}}

unavailable, nothing computable:
{"metric":"wait_time","version":1,"value":null,"unit":"ns","basis":"observed",
 "completeness":"unavailable","reasons":["evidence_invalid"],
 "diagnostic":{"recorded_ns":null,"covered_ns":null,"required_ns":null}}
```

- `value` is null if and only if `completeness` is `unavailable`. A measured zero is
  `value: 0, completeness: "complete"`.
- `detail` exists only for `wait_time` and only when complete. It is never `null`; it is absent.
- `diagnostic` always exists with its three keys (`recorded_count` replaces `recorded_ns` for
  `completed_waits`).
  - `required_ns` = length of `R`; `covered_ns` = length of `R ∩ coverage`.
  - `recorded_ns` / `recorded_count` = the section-4 formulas applied to ALL records in the
    evidence, covered or not. When the metric is unavailable this is a diagnostic only; a consumer
    must never present it as the metric.
- Reasons, sorted alphabetically, all that apply:

| reason | when | diagnostic values |
|---|---|---|
| `coverage_incomplete` | required evidence (section 4) is missing | numbers |
| `evidence_invalid` | section 2 is violated | all null |
| `invalid_query` | section 3 | all null |
| `invalid_window` | `from >= to` | all null |

  `coverage_incomplete` is reported together with `evidence_invalid` only when the lifetime and the
  coverage list are themselves valid (rules 1, 2, 7, 8), so that the gap is a fact and not a guess.
  It is never reported together with `invalid_window` or `invalid_query`.

## 6. Worked examples (hand-calculated; small numbers for readability)

Evidence E1: lifetime `[0, null)`, coverage `[[0, 20)]`, waits `PgSleep [0,6)`, `Lock [6,8)`,
`PgSleep [8,14)`, no open wait. Query event `PgSleep`.

| window | wait_time | completed_waits | why |
|---|---:|---:|---|
| `[4,10)` | 4 | 1 | `[4,6)` = 2 and `[8,10)` = 2; the close at 6 is inside, the close at 14 is not |
| `[4,6)` | 2 | 0 | the close at 6 is not `< 6` |
| `[6,10)` | 2 | 1 | the close at 6 belongs to this window |
| `[6,8)` | 0 | 1 | no PgSleep time in the window, but the close at 6 is in it |

Evidence E2: lifetime `[0, 10)`, coverage `[[0, 10)]`, wait `PgSleep [8,10)`. Query `PgSleep`.

| window | wait_time | completed_waits | why |
|---|---:|---:|---|
| `[10,12)` | 0 | 1 | `R` is empty, so no time; the close at the exit instant 10 is in the window and the span ending at the exit certifies it |

Evidence E3: as E2 but coverage `[]`. Window `[10,12)`: wait_time is 0 and complete (`R` is empty,
nothing is required); completed_waits is unavailable, `coverage_incomplete` (no span ends at the
exit), with `recorded_count` 1.

## 7. Clarifications (adopted after the second independent challenge round, 2026-11-11)

These change no expected answer of the 48 cases; they write down readings the oracle author had to
choose, so that an implementer cannot choose differently.

1. A coverage span may be empty (`a == b`). An empty span ending at `life_end` certifies closes at
   the exit and nothing else. Empty spans never overlap anything.
2. For deciding whether `coverage_incomplete` is also reported next to `evidence_invalid`, "valid"
   means: the lifetime is valid (rules 1-2 for its own fields), the coverage list is valid (rules 1
   and 7 for its own fields), and, if an open wait exists, its `as_of` is an in-range integer and
   rule 8 holds. Malformed waits elsewhere do not suppress a knowable gap.
3. Whenever any of `evidence_invalid`, `invalid_query`, `invalid_window` applies, all three
   diagnostic fields are null, even if `coverage_incomplete` is also reported.
4. Query reasons are independent: an empty event with `from >= to` gives
   `["invalid_query", "invalid_window"]`. `invalid_window` is decided only when both endpoints are
   in-range integers.
5. "Integer" means a JSON integer token. Booleans, strings and numbers written with a fraction or
   exponent (`1.0`, `1e0`) are not integers. All keys `life`, `coverage`, `waits`, `open` and
   `event`, `from`, `to` are required (`open` may be `null`); extra keys are ignored; list lengths
   are exact.
6. Records are never deduplicated. A collector that may re-deliver a record must give it an
   identity; that is a capture-side requirement, not something the core repairs.
7. The case names of `wait_window_43` and `wait_window_44` were wrong in the first draft; the fields
   govern, never the names. The names are corrected.
