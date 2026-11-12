# Contract v1: independent oracle challenges

This file was written first, after reading the contract and all 48 inputs and before
creating any expected-answer files or evaluator. The alternatives below are manual
readings, not results obtained from an implementation. No product was inspected.
Sections 2–5 of the contract take precedence over case names and informal wording.
Some entries are genuine underspecification; others are explicitly resolved by the
contract but remain misleading descriptions or product-design objections. They are
identified as such rather than presented as equally valid interpretations.

Notation: `T` is wait_time, `N` is completed_waits. `C(v)` means complete with value
v; `U(r)` means unavailable for coverage_incomplete with recorded diagnostic r.
`I(reasons)` means unavailable with the listed sorted reasons and all three
diagnostics null. `(covered, required)` gives the common time diagnostics. A complete
T has detail `(closed_ns, open_ns)`; unavailable results never have detail. Thus
these compact answers specify the material differences without duplicating the JSON
envelope. Case numbers below abbreviate `wait_window_NN`.

## Challenges

### C01 — Case names 43 and 44 contradict the data

Affected: 43, 44. This is a fixture-description defect, not a formula ambiguity.

* 43 says the window spans the exit, but `[3,12)` ends before exit 15. Using the
  fields gives T=C(2), N=C(1), diagnostics (9,9). If the label were taken to mean
  `[3,20)`, both become U, recorded 2 and 1 respectively, diagnostics (9,12).
* 44 says the exit is inside the window and uncertified. In fact 15 is excluded
  from `[12,15)`, and `[0,15)` certifies exit closes. Fields give T=C(3), N=C(0),
  diagnostics (3,3). Changing only `to` to 16 gives T=C(3), N=C(1), still complete.
  The named outcome T=C(3), N=U(1), diagnostics (3,3), would need a separate exit
  certification flag: it cannot be expressed with this lifetime and full time
  coverage under v1. Chosen: literal fields, never repair inputs from their names.

### C02 — Empty lifetime versus membership of a zero-length wait

Affected: 48; the same endpoint distinction appears in 12, 31, 32, 44.
"Lies in the lifetime" might suggest no record can exist in an empty half-open
set, or that a record at exit is outside it. The explicit inequalities allow
`start=end=life_start=life_end=7`. Chosen: inequalities govern. For 48, T=C(0),
N=U(1), diagnostics (0,0): the exit is in the window but not certified. Requiring
membership of the start instant in L instead gives both I(evidence_invalid).
For 12/31/32/44, the positive wait starts inside L and closes at the allowed exit;
requiring its close to be inside L would incorrectly invalidate these too.

### C03 — Empty coverage spans can certify exit closes

Affected existing cases: none directly; 32 and 48 lack such a span and motivate
this missing boundary. All coverage spans may have a=b. Section 4 requires only
a span *ending* at exit, with no positive-length qualification. Chosen: adding
`[10,10]` to 32 makes N=C(1), T=C(0), diagnostics (0,0); adding `[7,7]` to 48
does the same. Reading an empty span as certifying nothing leaves N=U(1).
This should be stated explicitly: empty state coverage can carry nonempty close
information. Duplicate empty spans and empty spans inside positive coverage have
no duration and do not violate the positive-interior overlap rule.

### C04 — Coverage certifies exit closes outside its half-open occupied span

Affected: 12, 31, 32, 44, 48. This exception is explicit, but the word "coverage"
usually suggests a set of covered instants. Chosen: keep the separate exit test.
12 gives (T,N)=(C(3),C(1)); 31 gives (C(0),C(1)); 32 gives (C(0),U(1));
44 gives (C(3),C(0)); 48 gives (C(0),U(1)). A state-only coverage test would
give C(1) for N in 32 and 48, despite no certified exit. Requiring ordinary
half-open coverage of exit instead would give U(1) for N in 12 and 31, even
though `[0,exit)` explicitly certifies it. 44 needs no exit certification since
exit equals `to`. There is no coherent alternate v1 formula here; this is a
representation/design caveat that explains the asymmetric metrics.

### C05 — "Still in wait at as_of" versus an exclusive observation endpoint

Affected: 15–18, 37, 38, 41, 42. The language describes state at as_of, but then
expressly excludes `[as_of,as_of+1)`. Chosen: that exclusion and rule 8 govern.
15: T=C(11), detail (3,8); 16: T=C(3), detail (0,3); 17: T=U(6);
18: T=C(3); 41: T=C(6); 42: T=U(6). N is C(1), C(0), U(0), C(1),
C(0), U(0), respectively. Extending occupied time by one tick would change only
17 and 42 among these valid cases: recorded_ns becomes 7, still unavailable
with unchanged coverage. If coverage also gained that tick, (covered,required)
would change from (6,13) to (7,13) in 17 and (8,10) to (9,10) in 42.
37 remains invalid because coverage ends at 10, beyond even 5+1; 38 remains
invalid because a closed record ends after the open start. There is no inferred
close, including when `start=as_of` (untested by the 48).

### C06 — An open record can coexist with a known exit and later gaps

Affected: 41, 42; rule 8 also matters to 37, 38. "Open" can mean either an
unfinished wait now or an unclosed historical observation. Chosen: the latter,
as rule 6 expressly permits `as_of < life_end`. 41 gives (C(6),C(0)), (8,8);
42 gives (U(6),U(0)), (8,10). Requiring `life_end=null` for an open record
would make both I(evidence_invalid). Inventing a close at exit instead would
change 42's recorded diagnostics to 8 ns and 1 close, but would still leave
both metrics unavailable without extra coverage. Rule 8 globally rejects even
later independently certified coverage: 37 is I(evidence_invalid), whereas
trusting its `[0,10)` span despite the open record would produce C(3), C(0).
38 is I(evidence_invalid); accepting the disjoint later closed wait despite
rule 6 would produce C(2), C(0). These global invalidations can discard useful
historical measurements; v1 deliberately chooses consistency over salvage.

### C07 — Scope of validation for the extra coverage_incomplete reason

Affected: 35, 39; also any malformed-wait case combined with a gap, absent from
the supplied set. Section 5 says the lifetime and coverage list must themselves
be valid, then cites rules 1,2,7,8; rule 1 also checks every wait's shape/time/event.
Chosen: rule 1 is scoped to the lifetime, coverage, and the open observation
information needed for rule 8 when deciding if a gap is knowable. Full evidence
validity is checked separately. 35 has valid coverage [0,4) and a 6 ns gap:
both I(coverage_incomplete,evidence_invalid). 39 has no gap: both
I(evidence_invalid) under either reading. If 39 instead had empty coverage,
the chosen reading would give both reasons; demanding global rule-1 validity
would give only evidence_invalid. A malformed open record without an interpretable
as_of cannot certify that the coverage list obeys rule 8; omit the gap reason.
This latter combination has no supplied case and is a conservative explicit choice.

### C08 — Diagnostic-null precedence when multiple reasons apply

Affected: 35, 36. The reason table says coverage_incomplete has numeric diagnostics,
but evidence_invalid has all null. Chosen: any evidence/query/window invalidity
makes all three diagnostics null, while retaining all applicable reasons. Thus
35 has both reasons and all null, not partial diagnostics such as recorded_ns=null,
covered_ns=4, required_ns=10 (nor the computable but invalid-record sum 5).
36 has [evidence_invalid,invalid_window], all null. A fail-fast interpretation
would report only invalid_window (or only evidence_invalid); "all that apply"
rules that out. No coverage_incomplete accompanies either query/window error.

### C09 — Query error combinations and JSON scalar types need a precise schema

Affected existing cases: 23, 36, 39, 45 (the broader combinations are missing).
23 gives only invalid_window; 36 gives evidence_invalid and invalid_window;
39 gives only evidence_invalid; 45 gives only invalid_query. Chosen: all
independently decidable reasons, with from>=to tested only on in-range integer
endpoints. For an empty event and from=to=10, both metrics have
I(invalid_query,invalid_window); treating invalid_query as a short circuit would
report only invalid_query. A negative endpoint with from=-1,to=-2 is invalid_query
only under our range-before-window choice; applying comparison to invalid times
would add invalid_window. No supplied case chooses between these last readings.
"Integer" is interpreted as a JSON integer token: booleans, strings, 1.0 and 1e0
are rejected. Mathematically integral JSON numbers could reasonably be accepted
instead, giving ordinary results. A string event must be nonempty, including for
evidence. Whitespace is nonempty and comparison is exact. All evidence keys
life/coverage/waits/open and query keys event/from/to are required; a missing open
is invalid rather than defaulting to null. Extra object keys are ignored, because
no prohibition is specified; list arities are exact. These are explicit schema
choices for unspecified JSON details, not additional failures in the 48.

### C10 — Duplicate records versus duplicate observations

Affected: 33, 34, 40. Chosen: records are distinct as explicitly stated, never
deduplicate them. 34 gives T=C(2), N=C(3); deduplicating zero-length records
would give N=C(2). 33 is I(evidence_invalid) for both metrics because the zeros
are strictly inside [2,8); treating them as empty sets exempt from rule 5 would
give C(6), C(3) (or C(2) for N after deduplication). 40 is invalid because
positive duplicate intervals overlap; deduplicating first would instead give
T=C(18446744073709551615), N=C(0). A real collector needs event identity or
delivery guarantees: a retry must not masquerade as another zero-length wait.

### C11 — "Time waited" is not latency, session totals, or an indication of trouble

Affected: 01–09, 18, 46 illustrate event selection; all 48 use one generation.
Chosen: exact event and backend scope, as the contract requires. For 01/46,
PgSleep gives T=C(4), N=C(1), whereas aggregating all named events gives
T=C(6), N=C(2). For 05, Lock gives C(2), C(1), whereas all events give
C(12), C(3). For 06, IO gives C(0), C(0), versus those same all-event totals.
For 18, PgSleep gives T=C(3), N=C(1), whereas all events including the open
Lock give T=C(11), N=C(1). A literal session-wait label would be wrong: no
connection identity across backend generations, query association, cause, or
active-versus-idle distinction is provided. An idle/sleep label can accumulate
time without establishing query delay. Values for a logical session or a query
cannot be derived at all from these records. The definition already acknowledges
this limitation; it should remain visible in the reporting tool.

For completeness, the other event-selection examples have these all-event
alternatives: 02 stays (C(2),C(0)); 03 changes from (C(2),C(1)) to
(C(4),C(2)); 04 changes from (C(0),C(1)) to (C(2),C(1)); 07 stays
(C(3),C(0)); 08 stays (U(4),U(1)); 09 stays (U(0),U(0)). Coverage
diagnostics are unchanged by event selection in all these comparisons.

### C12 — Completed waits and occupied time describe different populations

Affected: 02–04, 07, 16, 19–22, 31, 32, 44. Chosen: close timestamp selects
N independently of occupied time. For 04, (T,N)=(C(0),C(1)); counting only
waits occupying W gives N=C(0). For 07, (C(3),C(0)); counting intersecting
closed waits gives N=C(1). 02 gives (C(2),C(0)) versus intersecting N=C(1);
03 gives (C(2),C(1)) versus intersecting N=C(1) but for a different wait;
16 gives (C(3),C(0)) versus N=C(1) if open records were counted as waits.
19 gives (C(1),C(1)) versus intersecting closed N=C(1), again a different
record. 20 gives (C(0),C(0)) under both. 21 gives (C(2),C(1)) versus
intersecting N=C(2); 22 gives (C(1),C(0)) versus intersecting N=C(1).
31 gives (C(0),C(1)) versus intersecting N=C(0); 32 retains N=U(1), but
the alternate recorded_count would be 0; 44 gives (C(3),C(0)) versus N=C(1).
Consequently T/N is not the average duration of completed waits. No such derived
average is authorized; for 04 the actual finished wait lasted 6 ns despite T=0.

### C13 — Recorded observations outside coverage remain real but insufficient

Affected: 08, 09, 13, 17, 28, 32, 42. Chosen: completeness gates the value,
not the recorded diagnostic. In 28, T=U(3), N=U(1), (0,10). Ignoring
uncovered records gives diagnostics 0 and 0; promoting exact records to a
complete history gives C(3), C(1). Both alternatives violate explicit clauses.
For the other affected cases the chosen recorded pairs are 08=(4,1), 09=(0,0),
13=(5,2), 17=(6,0), 32=(0,1), 42=(6,0); promoting those diagnostics would
incorrectly make all unavailable metrics complete. A tool could separately
expose lower-bound observations, but the contract prohibits displaying them as
the metric. Requiring coverage even where a known wait occupies the entire
window is conservative: full exact occupation already determines event duration,
but v1 still requires the independent completeness certificate (missing case M07).

### C14 — Maximum timestamp excludes the last possible instant from every window

Affected: 26, 40. Chosen: unsigned 64-bit timestamps, exclusive `to`, exact
unbounded intermediate integer arithmetic, with no invented 2^64 endpoint.
26 has T=C(18446744073709550999), N=C(1), covered=required=
18446744073709551615. 40 is I(evidence_invalid), never an overflowing sum.
An inclusive reading of the maximum endpoint would change 26's required/covered
duration to 18446744073709551616 if coverage were also inclusive, while its
wait time remains 18446744073709550999. Such a special case is not authorized.
No legal query can count a close at 18446744073709551615: it would need a larger
exclusive upper bound. This is a domain expressiveness limit, not a reason to
change the given answers. A successor sentinel would solve it in a future version.

### C15 — Evidence validity is global; sorting, clipping, or filtering is not repair

Affected: 24, 25, 30, 33, 35–40, 47. Chosen: validate the entire generation,
including events and records outside the query. Each is evidence_invalid;
35 also has coverage_incomplete and 36 also invalid_window. Filtering 24 to
PgSleep first would give C(6), C(1) instead of two invalid results. Ignoring
38's later Lock would give C(2), C(0). Sorting 30's coverage would give C(0),
C(0), diagnostics (10,10). Clipping 47's coverage to lifetime would give
C(3), C(1), diagnostics (15,15). These are tempting ingestion normalizations,
but are not contract evaluation. Wait order itself has no meaning: 46 equals
01, C(4), C(1), diagnostics (6,6). A tool may choose to normalize before
constructing evidence, but then it is evaluating different input.

## Self-disagreements

Initial pre-comparison commitment: record any hand/reference disagreement here
with the original answer, reference answer, adjudication, and reason before
either is corrected.

Final audit: the first run of `python3 -B reference/run_all.py` matched all 48
hand answers exactly, including reasons, diagnostics, and absent detail keys.
No hand answer or reference result was corrected following comparison.
Self-disagreements: **0**. Cases 26 and 40 used the explicitly reported integer-interval
fallback; 23, 36 and 45 stopped at query/window validation; the other 43 walked
every nanosecond. Case 40 still returns null diagnostics because its evidence is
invalid: choosing the large-window path does not make invalid records computable.

Final suite line:
`RESULT: 48/48 cases matched; 0 failures; 2 exact-integer fallbacks.`
There are **15 challenges** (C01–C15), counting resolved descriptive/design issues
as well as genuine underspecification, and ten missing-case proposals below.
All ten proposal JSON blocks were also parsed and checked against their documented
manual predictions; all matched. They remain outside the fixed 48-case suite.

## Missing cases

The following ten proposals exercise behavior not isolated by the supplied 48.
These are proposals, not additions to the fixed 48-case comparison suite. Unless
noted, no alternative to the chosen readings above is implied.

### M01 — An empty span certifies exit, including an empty generation (C02/C03)

Expected T=C(0), N=C(1), diagnostics (0,0). Catches dropping empty coverage
spans before the exit certificate test; existing 48 has no certificate.

```json
{"id":"missing_01","name":"empty exit certificate","evidence":{"life":[7,7],"coverage":[[7,7]],"waits":[["PgSleep",7,7]],"open":null},"query":{"event":"PgSleep","from":0,"to":20}}
```

### M02 — Empty coverage inside a positive span and tied starts

Expected T=C(0), N=C(0), diagnostics (10,10). Catches treating empty spans
as overlapping, or using the immediately preceding end as the union frontier.

```json
{"id":"missing_02","name":"empty spans inside coverage","evidence":{"life":[0,null],"coverage":[[0,0],[0,10],[5,5]],"waits":[],"open":null},"query":{"event":"PgSleep","from":0,"to":10}}
```

### M03 — Zero-length records allowed at wait and open boundaries

Expected T=C(8), detail (4,4), N=C(3), diagnostics (8,8). Catches changing
the strict rule-5 inequalities to inclusive ones or rejecting end=open.start.

```json
{"id":"missing_03","name":"boundary zero waits and open handoff","evidence":{"life":[0,null],"coverage":[[0,8]],"waits":[["PgSleep",0,0],["PgSleep",0,4],["PgSleep",4,4]],"open":["PgSleep",4,8]},"query":{"event":"PgSleep","from":0,"to":8}}
```

### M04 — Query-invalid and window-invalid can coexist (C09)

Expected both I(invalid_query,invalid_window), with no coverage reason.
Catches returning after the first query failure.

```json
{"id":"missing_04","name":"two query reasons","evidence":{"life":[0,null],"coverage":[],"waits":[],"open":null},"query":{"event":"","from":10,"to":10}}
```

### M05 — Malformed wait time does not prevent an independently knowable gap (C07)

Expected both I(coverage_incomplete,evidence_invalid). Catches applying all of
rule 1 globally to the coverage-diagnostic gate; existing 35 only breaks overlap.

```json
{"id":"missing_05","name":"bad wait timestamp and good empty coverage","evidence":{"life":[0,null],"coverage":[],"waits":[["PgSleep",-1,1]],"open":null},"query":{"event":"PgSleep","from":0,"to":2}}
```

### M06 — A JSON boolean is not an integer timestamp (C09)

Expected both I(invalid_query). Catches Python's bool-is-an-int trap. A permissive
boolean-as-zero interpretation instead gives C(0), C(0), diagnostics (2,2).

```json
{"id":"missing_06","name":"boolean query timestamp","evidence":{"life":[0,null],"coverage":[[0,2]],"waits":[],"open":null},"query":{"event":"PgSleep","from":false,"to":2}}
```

### M07 — Fully occupied exact interval still requires coverage (C13)

Expected T=U(10), N=U(0), diagnostics (0,10). Catches deriving completeness
from positive observed occupation; 28 leaves unobserved time around its wait.

```json
{"id":"missing_07","name":"occupation does not imply certification","evidence":{"life":[0,null],"coverage":[],"waits":[["PgSleep",0,10]],"open":null},"query":{"event":"PgSleep","from":0,"to":10}}
```

### M08 — Empty open observation contributes no duration or close

Expected T=U(0), N=U(0), diagnostics (0,1). Catches treating start=as_of as
a closed zero-length wait (which incorrectly records one close), extending it
by a tick (which incorrectly records 1 ns), or disallowing that open shape.

```json
{"id":"missing_08","name":"zero-length open at window start","evidence":{"life":[5,null],"coverage":[],"waits":[],"open":["PgSleep",5,5]},"query":{"event":"PgSleep","from":5,"to":6}}
```

### M09 — Positive overlapping coverage is invalid even when no query gap exists

Expected both I(evidence_invalid). Catches union-normalizing overlapping spans;
30 only checks sorting, while 29 has touching, valid spans.

```json
{"id":"missing_09","name":"overlapping coverage","evidence":{"life":[0,null],"coverage":[[0,6],[5,10]],"waits":[],"open":null},"query":{"event":"PgSleep","from":0,"to":10}}
```

### M10 — Event comparison is case sensitive, including open waits

Expected T=C(0), N=C(0), diagnostics (10,10). Catches case folding; the 48
only distinguish wholly different event names.

```json
{"id":"missing_10","name":"case sensitive event","evidence":{"life":[0,null],"coverage":[[0,10]],"waits":[["PgSleep",0,4]],"open":["PgSleep",4,10]},"query":{"event":"pgsleep","from":0,"to":10}}
```
