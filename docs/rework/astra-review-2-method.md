<!-- Review by the Astra design adviser, 2026-11-11, read-only; reproduced verbatim except that local absolute paths were made repository-relative. -->

**INFERRED — Rebuild the accounting boundary incrementally; do not authorize a wholesale replacement of the middle yet.** The first investment should establish whether a small replacement can deliver an independently correct answer through the existing product at an acceptable cost in agent rounds.

**INFERRED — The plan in ten lines:**

1. Define a small set of numerical contracts, starting with one wait’s time and completion count.
2. Have a separate oracle author derive expected answers without access to the implementation.
3. Freeze those answers before the implementer starts; implementation changes cannot change acceptance values.
4. Preserve useful infrastructure, but qualify each component for a specific contract rather than declaring whole layers “solid.”
5. Build a portable C library whose consumers receive finished metrics, coverage and reasons.
6. Keep observation, estimation, attribution and missing evidence distinct in that library.
7. Route one existing view through it while the old path remains available for migration comparisons.
8. Require deliberately broken implementations to fail the independent tests and the real server-to-view path.
9. Migrate one numerical family at a time, deleting its previous accounting authority as it switches.
10. Expand only after the first slice establishes actual token cost, review cost and box usage.

**INFERRED — First slice:** one backend generation, one wait event, exact evidence, arbitrary bounded windows: **wait time and completed-wait count**. Include a wait closing after the requested window, a completion exactly on a boundary, legitimate zero, missing coverage and corrupt input. Run the new core → server adapter → Go bridge → one existing view against the same trace files as the old implementation. Budget **two implementation units and two reviews**, plus **two specification/oracle authoring rounds** and **1–2 box-hours** for a narrowly scoped capture check. That is a pilot budget, not a promise.

**INFERRED — The three likeliest failures:**

- **The inputs cannot answer the question.** Better arithmetic cannot reconstruct lost sample timing, missing category history or an unrecorded open interval.
- **Independence becomes ceremonial.** A second agent copies the same assumptions, generates expected values from product output, or relaxes the contract after seeing failures.
- **Migration becomes another feature programme.** New filters, summaries and optimizations arrive before old authorities are removed; review and compatibility work then dominate again.

**VERIFIED — Basis of this answer:** I re-read the earlier summary and inspected the current checkout, selected tests, CI configuration, measurement artifacts and the known-answer worker in its worktree. HEAD is `d7f045fd`. I made no file changes and ran no builds or qualification suites. “VERIFIED” below means read or counted, including explicitly identified recorded results; it does not mean I reproduced the capture.

---

**1. VERIFIED findings and INFERRED decisions about what to keep**

**INFERRED — “Proven solid” must name a bounded claim.** Successful attachment is evidence for attachment. Frozen-byte decoding is evidence for compatibility. Neither proves the correctness of DB Time. Nothing inspected justifies marking an entire capture, storage or presentation layer correct.

| Candidate | VERIFIED evidence and its limits | INFERRED decision and required check |
|---|---|---|
| **BPF capture and kernel integration, including EL8** | Exact-probe tests exercise attachment rollback, generations and capability policy using fake attachment operations. The EL8-labelled known-answer results contain **36 full-mode cells: 35 inside their recorded truth bounds, one outside**. The worker brackets real PostgreSQL wait-word changes. This is stronger evidence than tier parity, but is not a universal pass. [Probe tests](tests/test_exact_probe.c), [recorded cells](tests/results/session-20261111/m3/el8/cells.json). | Preserve the mechanism. Do **not** declare it qualified wholesale. Reproduce the outlying cell with pinned binaries; check event identity and placement as well as total duration. Add loss, attach/detach, backend exit/reuse and capability-absence cases on actual EL8. |
| **PostgreSQL layout knowledge, 13/17/18** | PG13 offsets are documented as header-derived; tests check constants, garbage rejection and that a zero wait word is inconclusive. Backend-status tests use synthetic DWARF layouts. These establish useful parser/refusal contracts, not correctness for every PostgreSQL build. [Discovery](src/discovery.c:791), [PG13 tests](tests/test_pg13_resolve.c), [layout tests](tests/test_backend_status_layout.c). | Keep the knowledge and validators. Qualify each supported build/ABI with a compiled layout probe or matching debug information, plus controlled nonzero waits and query/activity transitions. A plausible class byte alone is insufficient validation. |
| **Compressed trace-block format** | Frozen rev2/rev3 files detect decoding drift. Their expected checksums originated from product decoding; the README describes updating them from printed output. This is a compatibility oracle, not an independent byte-format oracle. [Fixture documentation](tests/fixtures/golden/README.md), [test](tests/test_golden_fixture.c). | Keep the container and legacy compatibility. Add a tiny independently encoded byte fixture with literal expected fields. Permit an additive evidence-format revision; do not freeze insufficient record semantics merely to preserve the container. |
| **Block decoder** | It checks varint/raw-column bounds and reports decoding errors. However, it caps `count` to `max_events` **before walking column boundaries**. [Decoder](src/event_reader.c:260). | Preserve as a candidate, not unquestioned “as-is” code. Specifically test a multi-column block with output capacity smaller than its event count: the current structure appears capable of treating remaining timestamps as the next column. Also test oversized sizes, timestamp overflow, malformed varints and unknown block types. Prefer whole-block decode or explicit insufficient-capacity refusal. |
| **Go bridge** | Tests cover concurrent clients using the same request IDs, upstream death/respawn, request-ID precision and refusal propagation. CI sets `PGWT_BRIDGE_TEST_REQUIRE=1`, making absent integration binaries fail rather than skip. [Tests](web/bridge_test.go), [CI](.github/workflows/ci.yml:277). | Keep. Run those tests against the new response schema, including unavailable values and integers above \(2^{53}\). Replace stderr-derived machine meaning with structured errors when touching that boundary. |
| **Interface views and charts** | Transport tests independently assert error rejection, cancellation and stale-response handling. Builder tests assert concrete chart configurations. They do not establish numerical truth upstream. [Transport tests](tests/web_unit/transport.test.mjs), [AAS tests](tests/web_unit/aas.test.mjs). | Keep rendering, navigation and useful interaction tests. Do not retain view-specific aggregation or quality interpretation automatically. Exercise unavailable/partial values through the actual bridge and rendered view, including an old delayed response arriving after a new request. |
| **Known-answer workload tool** | The worker brackets actual wait-word stores with monotonic timestamps and records thread CPU inside those brackets. Its Python truth calculation uses those boundaries, not requested sleep lengths or product totals. The driver explicitly says **“No accuracy tolerance and no PASS verdict.”** Its trace scanner uses the product decoder. [Worker](tests/known_answer_worker.c), [driver](tests/known_answer.py). | Keep and promote into a reproducible test package. Independently check its bound arithmetic on literal cases; pin worker, driver and binary hashes. Add an explicit acceptance policy. It is independent measurement evidence, but currently an experiment tool rather than a release gate. |
| **Closure/checkpoint infrastructure** | Tests exercise missing/corrupt inputs, failed publication and recovery. A recorded macOS closure run reports 91/91 assertions. The checkpoint contract explicitly excludes reconstructed time from ordinary totals. [Contract](src/open_ckpt.h), [recorded run](tests/results/issue424-durability/unit-test_trace_closure-mac.log). | Keep identity, durable-publication and refusal machinery. Re-test reconciliation against the new evidence model. Do not silently change checkpoint time into measured total time during extraction. |

**INFERRED — Specific pieces I would lift unchanged initially, with their existing preconditions made explicit:**

| Piece | VERIFIED support | INFERRED boundary of reuse |
|---|---|---|
| `event_window_ns`, `pgwt_cpu_clip_ns`, `pgwt_offcpu_ns` | Literal straddler answers, integer endpoint cases, zero-wall cases and partition tests exist. The CPU helper explicitly disclaims knowledge of where CPU ran within an interval. [Helpers](src/window_clip.h), [tests](tests/test_window_clip.c). | Lift the arithmetic bodies. Wrap legacy zero-as-unbounded arguments at the boundary. Validate inputs and aggregate overflow separately. Mark subinterval CPU allocation **estimated**, even when its whole-interval CPU counter was observed. |
| Stable event sorting | Tests include explicit tie-order expectations and deliberate tie/comparator failures, besides comparison with the previous sort. [Tests](tests/test_event_sort.c). | Lift for legacy event normalization. Do not assume physical input order resolves ambiguous semantic ordering between independently produced streams. |
| Integer/double ascending comparators and sort wrappers | Tests exercise sub-millisecond comparator failures; integer variants avoid subtraction overflow. [Implementation](src/percentile.h). | Lift with finite-value/non-NaN input requirements for doubles. |
| `pgwt_qid_marker_step` inside `compute.c` | It is already a small shared rule; left-edge attribution tests exercise command resets and stale seeds. [Rule](src/compute.c:524), [tests](tests/test_left_edge_attrib.c). | Lift unchanged for the existing marker semantics. Keep it separate from the decision whether historical query attribution is actually known. |
| Deterministic identity tie-breaks in result sorting | `compute.c` explicitly breaks equal-time ties by identity. [Comparator](src/compute.c:1877). | Preserve that policy and comparator logic when the corresponding row types move. |

**VERIFIED — I would not lift “the percentile code” as one unit.** `pgwt_percentile_at` deliberately preserves the old small-population rule: at or below the threshold it returns the maximum. The histogram implementation instead uses nearest-rank selection and exposes top-bucket overflow. These are different contracts. [Array percentile](src/percentile.h), [histogram percentile](src/compute.c:1827).

**INFERRED — Keep sorting; specify percentile populations, rank policy, censoring and histogram bounds before adopting selection logic.** Likewise, retain interval/execution indexes as optimization candidates. Their tests explicitly use shipping computation as an oracle, which is useful for selection equivalence but insufficient to establish the underlying metric definition. [Interval-index test](tests/test_interval_index.c), [execution-index test](tests/test_exec_index.c).

---

**2. INFERRED — Make construction rules executable**

**INFERRED — Use one small contract package per numerical family.** Proposed files:

```text
contracts/metrics/wait_time.yaml
contracts/examples/wait_window_01.json
contracts/expected/wait_window_01.json
contracts/acceptance/wait_time.yaml
tests/reference/time_grid.py
```

**INFERRED — A definition must specify:** metric ID and version; question answered; population; units; evidence required; time-window ownership; classification; estimator/allocation rule; missing-data behavior; valid bounds; supported filters; and rounding. “DB Time = CPU + waits” is not a sufficient definition.

**INFERRED — The first worked example should look like this:**

```yaml
metric: wait_time
version: 1
population: backend_generation=g7, event=PgSleep
window: "[from_ns,to_ns)"
time_rule: duration of intersection with the requested window
completion_rule: count a genuine close iff from_ns <= close_ns < to_ns
requires: complete exact wait-state coverage for the selected population
missing_coverage: unavailable; expose recorded subtotal only as a diagnostic
```

| INFERRED example; times shown in milliseconds | Expected wait time | Expected completed waits |
|---|---:|---:|
| PgSleep `[0,6)`, Lock `[6,8)`, PgSleep `[8,14)`; query `[4,10)` | 4 ms | 1 |
| Same evidence; query `[4,6)` | 2 ms | 0 |
| Same evidence; query `[6,10)` | 2 ms | 1 |
| Same evidence; query `[6,8)` | 0 ms | 1 |

**INFERRED — The last row is intentional:** a completion at 6 belongs to `[6,8)`, although that window contains no PgSleep duration. It prevents an implementer from deriving counts from positive time contributions.

**INFERRED — Ownership should be mechanical:**

- A designated contract author writes the definition and hand calculation before implementation. The lead resolves product meaning; the owner is involved only when that meaning or release scope changes.
- A separate oracle author receives the contract and evidence-format description, not product code or product outputs.
- The implementer can add diagnostic tests but cannot edit accepted definitions, expected answers or tolerances in the implementation change.
- A semantic change is a separate contract revision. It records why the previous answer was wrong and invalidates dependent acceptance results.
- CI checks protected-file changes against the trusted base revision. Agent-authored prose saying “approved” is not authorization.

**INFERRED — Enforce oracle isolation through access, not a prompt.** Give the oracle author a separate workspace containing only the contract bundle and allowed external truth inputs, with no repository/code access through its tools. Starting another agent in the same unrestricted checkout and saying “do not peek” does not establish independence.

**INFERRED — Use three different sources of expected values:**

1. **Literal tiny timelines:** a human-readable derivation and frozen integer answers.
2. **A slow Python reference:** enumerate occupied time cells on small integer timelines, and count endpoint events separately. Use arbitrary-precision arithmetic. It must not import product modules, translate the C reducer, or consume product results.
3. **Measured capture truth:** the known-answer worker’s actual transition brackets and CPU measurements. Requested workload durations are not truth.

**INFERRED — The reference implementation should validate the specification, not become a second production engine.** Large-value arithmetic gets separate exact integer/rational cases. Storage decoding gets independently constructed bytes. The live workload checks acquisition fidelity. No single oracle should be asked to prove all three.

**INFERRED — Enforce one numerical authority through these build boundaries:**

- Build the core as a separate target. Adapters cannot include its internal evidence/reducer headers.
- Make contribution, classification and aggregation functions private. Check exported symbols against a short allowlist.
- Server and CLI obtain opaque results through the same public API; they never receive raw interval arrays for accounting.
- Generate or centrally implement serialization from the metric schema. Adapter code cannot populate numeric metric fields itself.
- Introduce import/dependency checks for frontend views. Only shared metric rendering code accesses numeric value representations; views request formatted values, chart points and already-computed percentages.
- Add a targeted AST check rejecting arithmetic on evidence-duration fields outside the core’s accounting modules. Explicitly exempt capture encoding and independently maintained test references.
- Test the guard itself: introduce a forbidden include, reducer symbol, adapter sum and frontend metric recomputation; each must fail the architecture check.

**INFERRED — These constraints prevent ordinary accidental duplication; they do not prove that arbitrary code contains no disguised arithmetic.** Keep the checks small and auditable. A huge custom semantic linter would become another expensive product. The strongest defense is that consumers lack the inputs needed to rederive the metric.

**INFERRED — A unit of work is done when one contract increment reaches one consumer and its previous authority is removed or explicitly remains outside the migrated route.** Its review packet contains:

- The unchanged contract and literal expected answers.
- A narrow implementation diff.
- Passing independent and integration results tied to source/binary/input hashes.
- The required mutants, each rejected for the intended reason.
- Ownership, allocation-failure and work-budget behavior.
- A named deletion or explicit migration boundary.

**INFERRED — Size each unit for one review round, but never manufacture that outcome.** One Claude reviewer receives this packet once. A blocking finding means a correction and another charged round; record it as a sizing/process miss. Do not seek several advisers until their opinions converge.

**INFERRED — The implementer brief should contain:** contract/version, API boundary, allowed scope, supported input profile, acceptance commands, required mutants, resource limits, deletion target and unresolved questions. It should **not** contain old product totals as expected values, instructions to “make parity green,” speculative implementation pseudocode masquerading as a specification, or permission to regenerate acceptance outputs.

**INFERRED — Required mutation families, introduced as their functionality lands:**

| Family | Mutants that must fail |
|---|---|
| Harness | Return constant zero; always refuse; skip the fixture; substitute the wrong binary; remove the positive-control observation. |
| Windows and identity | Use full duration instead of overlap; drop a later-closing straddler; make completion upper bound inclusive; shift time to an adjacent bucket while preserving totals; conflate reused PIDs. |
| Evidence quality | Missing category becomes foreground; missing CPU becomes zero; unknown coverage becomes complete; decode failure becomes empty success. |
| Mixed sampling/exact | Replace elapsed weight with nominal weight; double-count exact-covered sample support; suppress samples across an exact-coverage gap; relabel allocated time observed. |
| Open state | Count checkpoint and close twice; use a stale checkpoint; extrapolate beyond its watermark; change recorded totals merely by adding the checkpoint sidecar. |
| Attribution/distributions | Move contributions to the wrong query while preserving totals; use sample count as wait count; use a wrong histogram population; render an open top bin as an exact percentile. |
| Consumers/rollups | UI unavailable→zero; adapter recomputes a percentage; stale reply overwrites current data; summary answers a filter or partial bucket it cannot represent. |

**INFERRED — A mutant counts as killed only if it builds, reaches the intended case and fails the expected assertion.** Patch failure, compilation failure, timeout and an unrelated crash are not successful detection.

---

**3. INFERRED — The core’s shape**

**INFERRED — Keep the five responsibilities, but distinguish pure calculation from storage I/O.** They need not begin as five elaborate frameworks.

| Module | Responsibility |
|---|---|
| `trace_store` | Read a committed snapshot; decode supported formats; report corruption, limits and provenance. Filesystem access lives here. |
| `capture_context` | Resolve backend generations, category/query validity ranges, event dictionaries and capabilities. Unknown remains explicit. |
| `interval_model` | Normalize evidence and reconcile overlaps/open records. Establish contribution support and occurrence identity without a requested-view dependency. |
| `metrics` | Apply the declared numerical contracts; aggregate contributions; produce values, bounds and quality. |
| `query_service` | Validate requests, select a supported evidence source, apply budgets and return immutable results. Later owns raw/rollup selection. |

**INFERRED — The public API should resemble the following.** This is a proposed starting contract, not existing code. Keep evidence structures out of consumer headers.

```c
/* pgwt_core.h */
#include <stddef.h>
#include <stdint.h>

typedef struct pgwt_snapshot pgwt_snapshot;
typedef struct pgwt_result pgwt_result;

typedef struct {
    uint64_t from_ns, to_ns;       /* finite, explicit [from,to) */
    uint64_t clock_domain;
} pgwt_window;

typedef struct {
    uint64_t capture_id;
    uint64_t generation;
    uint32_t pid;                 /* display identity, not sufficient alone */
} pgwt_backend_key;

typedef enum {
    PGWT_OBSERVED,
    PGWT_ESTIMATED,
    PGWT_UNAVAILABLE
} pgwt_value_state;

typedef enum {
    PGWT_COMPLETE,
    PGWT_PARTIAL,
    PGWT_COVERAGE_UNKNOWN
} pgwt_completeness;

typedef enum {
    PGWT_BASIS_INTERVAL,
    PGWT_BASIS_COUNTER,
    PGWT_BASIS_WEIGHTED_SAMPLE,
    PGWT_BASIS_PROPORTIONAL,
    PGWT_BASIS_HISTOGRAM
} pgwt_basis;

typedef enum {
    PGWT_UNIT_NS,
    PGWT_UNIT_COUNT,
    PGWT_UNIT_RATIO
} pgwt_unit;

typedef enum {
    PGWT_BOUND_NONE,
    PGWT_BOUND_RANGE,
    PGWT_BOUND_LOWER,
    PGWT_BOUND_UPPER
} pgwt_bound_kind;

typedef struct {
    uint64_t numerator, denominator; /* denominator > 0 */
} pgwt_number;

enum pgwt_reason {
    PGWT_R_MISSING_COVERAGE   = UINT64_C(1) << 0,
    PGWT_R_CATEGORY_UNKNOWN  = UINT64_C(1) << 1,
    PGWT_R_CPU_UNKNOWN       = UINT64_C(1) << 2,
    PGWT_R_OPEN_TAIL         = UINT64_C(1) << 3,
    PGWT_R_LEGACY_TIMING     = UINT64_C(1) << 4,
    PGWT_R_PHASE_ABSENT      = UINT64_C(1) << 5,
    PGWT_R_FILTER_UNSUPPORTED= UINT64_C(1) << 6,
    PGWT_R_HISTOGRAM_OVERFLOW= UINT64_C(1) << 7
};

typedef struct {
    uint32_t metric_id, definition_version, population_id;
    pgwt_value_state state;
    pgwt_completeness completeness;
    pgwt_basis basis;
    pgwt_unit unit;
    uint64_t reasons;
    uint64_t coverage_id;          /* references coverage spans in result */
    pgwt_number value;             /* valid only when state != UNAVAILABLE */
    pgwt_bound_kind bound_kind;
    pgwt_number lower, upper;      /* valid according to bound_kind */
} pgwt_metric;

typedef struct {
    uint64_t max_input_bytes, max_records, max_work_units;
} pgwt_limits;

typedef struct {
    uint32_t code;
    uint64_t source_id, record_ordinal;
} pgwt_error;

typedef struct {
    pgwt_window window;
    pgwt_backend_key backend;
    uint32_t event_id;
    uint64_t metric_mask;          /* first slice: time and completion count */
} pgwt_wait_query;

int pgwt_query_wait(const pgwt_snapshot *,
                    const pgwt_wait_query *,
                    const pgwt_limits *,
                    pgwt_result **out,
                    pgwt_error *);

size_t pgwt_result_metric_count(const pgwt_result *);
const pgwt_metric *pgwt_result_metric(const pgwt_result *, size_t);
void pgwt_result_free(pgwt_result *);
```

**INFERRED — A separate storage header should expose snapshot acquisition, without leaking decoder arrays:**

```c
/* pgwt_store.h; pgwt_source supplies immutable, bounded read_at operations */
typedef struct pgwt_source pgwt_source;

int pgwt_snapshot_load(const pgwt_source *,
                       const pgwt_limits *,
                       pgwt_snapshot **out,
                       pgwt_error *);
void pgwt_snapshot_free(pgwt_snapshot *);
```

**INFERRED — Contract details matter more than the names:**

- Failure leaves `*out == NULL`; never return a partially initialized successful result.
- Query-level corruption, allocation failure and exhausted processing budget are structured failures. Missing measurement capability can yield an unavailable metric in an otherwise valid result.
- `observed` describes measurement basis, not infallibility or complete population coverage.
- Measured zero is a valid value. Unavailable has no serialized value.
- Sample estimates and proportional CPU placement remain estimated.
- A partial positive subtotal can support a lower bound only when the missing contributions are known nonnegative. Ratios and percentiles do not inherit that bound.
- Coverage is a set of spans with reasons, not merely a percentage.
- Use integer/rational values internally, checked aggregate arithmetic and explicit overflow failure. Serialize large integers as decimal strings.

**INFERRED — The canonical internal evidence model needs these distinct record kinds:**

| Evidence kind | Required information |
|---|---|
| Exact interval | Backend generation, stable interval identity, start/end, wait-state identity, endpoint provenance, optional associated counter measurements. |
| Point observation | Actual observation timestamp or read bracket; observed state; batch/sequence identity; attribution snapshot and its validity. |
| Sampling support | Weight, represented time support, estimator version and timing quality. It must not be confused with the instant of observation. |
| Counter span | Counter identity, start/end observations, delta, availability and uncertainty. Scheduler CPU is separate from PostgreSQL wait state. |
| Marker/context change | Command/plan/execution identity, query/category value, validity boundary and source. |
| Coverage/capability span | Which evidence stream is trustworthy over which interval; loss, unavailable probes and metadata gaps. |
| Open checkpoint/close | Common interval identity, checkpoint watermark, committed-prefix identity and reconciliation provenance. |

**INFERRED — Every record also needs a source reference:** capture/file identity, ordinal and clock domain. Legacy records can have unknown generation or observation timing; the loader must not invent those facts.

**INFERRED — Normalize independently of the viewport.** Do not rewrite canonical interval endpoints when a request clips them. Window clipping belongs in contribution calculation. Maintain exact-coverage intervals explicitly; “first exact event through last exact event” is not necessarily continuous exact coverage.

**INFERRED — PostgreSQL wait state and scheduler state must remain separate axes.** “No wait reported” is not synonymous with “executing on a CPU.” CPU measured while reporting a wait is a useful diagnostic, but adding it to wait-wall time would double-count an exclusive DB-Time partition.

**INFERRED — The server becomes request parsing, snapshot/cache ownership, dispatch, cancellation and serialization.** It contains no classification, clipping, accumulation or percentile decisions. The CLI keeps argument parsing and presentation, and calls the same service. The Go bridge transports typed envelopes. The UI formats typed values and plots supplied points; core results supply totals, remainders, percentages and comparison deltas.

**INFERRED — First-version exclusions:** no summary input, live overlay, sample/exact blending, query/category filters, SQL-text attribution, percentiles or migration of all views. Only immutable raw traces, bounded queries, one backend/event selector and one view. Unsupported requests fail explicitly. These are pilot exclusions, not silent permanent feature cuts.

**INFERRED — Keep C for this migration.** The existing C decoder, arithmetic helpers and daemon interface make that the smallest integration change. Restrict the core to portable C11, explicit ownership, checked arithmetic and bounded allocation; use sanitizers and decoder fuzzing. Rust could improve memory-safety guarantees, but it would not resolve numerical definitions or missing evidence. A language change needs its own measured justification, rather than being bundled into this pilot.

---

**4. INFERRED — What the first slice must prove**

**INFERRED — Implement the example above through the existing product boundary.** Use a new versioned request and a small panel in the existing Events view. The panel identifies backend generation, event and window. It displays wait time and completed waits, or the reason they are unavailable.

**INFERRED — The fixture pack should contain:**

- The literal timeline above, independently encoded into an existing supported trace format.
- Different physical block partitions and commit orders carrying the same evidence.
- A close at 14 that must contribute time to `[4,10)`.
- A genuinely covered zero-duration-contribution window.
- Missing coverage and an unfinished tail.
- A checkpoint whose presence does not silently increase recorded totals.
- A malformed block and an exhausted read budget.

**INFERRED — On incomplete coverage, the full requested total is unavailable.** A diagnostic can say “2 ms recorded; coverage incomplete,” with checkpoint evidence separately identified. It must not present 2 ms as the complete wait time, or absence as zero.

**INFERRED — Required demonstrations:**

1. The hand calculation, independent reference and new core agree.
2. The actual browser receives the expected values and reasons through the server and bridge.
3. A boundary-placement mutant fails even though its whole-trace total remains unchanged.
4. Corruption and missing evidence survive every boundary as refusal/unavailability.
5. No consumer computes either metric independently.
6. The same snapshot produces the same result regardless of block partitioning.
7. Old/new differences are reported with an explanation, not automatically treated as new-code failures.

**INFERRED — Execution budget:**

| Work | Expected rounds |
|---|---:|
| Contract and literal cases | 1 authoring |
| Isolated oracle and acceptance fixtures | 1 authoring |
| Core/storage boundary and numerical contract | 1 implementer + 1 reviewer |
| Server/bridge/view integration and mutants | 1 implementer + 1 reviewer |

**INFERRED — macOS runs:** reference evaluation, decoder/core tests, sanitizers where supported, server replay, Go tests and browser integration. **Linux needs:** one pinned known-answer capture and the narrow acquisition-to-new-route check; budget 1–2 occupied box-hours, excluding provisioning. Reuse already captured evidence for ordinary development iterations.

**INFERRED — Stop expansion if the pilot needs a third implementation/review cycle because the numerical contract keeps changing; if the oracle author needs product code to derive the answer; if a mandatory mutant survives; or if routine arithmetic work still needs box experiments.** Those are signs that the proposed method or task boundary has not worked. An identified missing source field is a different result: it demonstrates that an evidence-format slice is necessary before further migration.

---

**5. INFERRED — Sequence after the pilot**

**VERIFIED — `server.c` has 9,267 lines and `compute.c` 6,346: 15,613 together.** Summary handling is distributed across those files and separate reader/writer code. Therefore “15,600 lines plus 6,000 summary lines” should not be treated as two clean, additive replacement units.

**INFERRED — Use the following as conditional planning ranges.** One implementation round means one narrow brief producing a self-validated change; one review round means one reviewer evaluating that change. Each row contains several such units. The ranges assume reuse of existing infrastructure, no new product features, an agreed evidence contract and no replacement of the sampler mechanism.

| Slice | Switch over and delete | What remains usable | Implementer rounds; target reviews | Occupied box-hours |
|---|---|---|---:|---:|
| **Pilot** | New wait-time/count route and one panel; no global replacement yet. | Existing product plus opt-in new route. | 2; 2 | 1–2 |
| **Evidence/context** | Canonical generation, category coverage, sample timing/support and open-state reconciliation. Remove migrated paths’ ad hoc context loading. Add writer fields where required. | Legacy traces remain readable with explicit limitations; old endpoints remain. | 3–4; 3–4 | 4–8 |
| **DB Time, wait/CPU axes, AAS** | One contribution model feeds Overview and AAS. Remove their independent raw reducers and frontend numerical synthesis. | Other views remain on clearly identified legacy routes. | 3–4; 3–4 | 1–2 |
| **Events, sessions, queries and filters** | Group the same contributions; preserve unattributed buckets and explicit denominators. Delete migrated per-view accounting loops. | Main tables and filters run through the core; detailed execution views can remain legacy. | 3–4; 3–4 | 1–2 |
| **Executions and derived views** | Migrate execution/plan populations, distributions, timeline, concurrency, lock/interference/variant calculations in small units. Reuse indexes only after oracle checks. Delete corresponding handlers’ arithmetic. | Each view switches independently; capability absence stays visible. | 3–5; 3–5 | 2–4 |
| **CLI** | Move replay and dump outputs to shared results; delete independent replay accumulation and dump accounting. | Existing CLI presentation over the same definitions as the UI. | 2–3; 2–3 | 0–1 |
| **Rollups** | Materialize canonical sufficient statistics; replace summary selection and reduction. Delete old summary accounting only after supported long-window behavior is covered. | Raw remains a bounded fallback, never an unbounded promise. | 2–4; 2–4 | 1–3 |
| **Retirement** | Remove unused old compute code, summary route, dead flags and old-schema numerical adapters. Run final platform/resource checks. | One numerical authority; documented legacy evidence limitations. | 2–3; 2–3 | 2–4 |

**INFERRED — That is approximately 20–29 implementation rounds and the same number of targeted reviews, plus roughly 6–8 contract/oracle authoring rounds: 46–66 agent rounds before correction allowance, and 12–26 box-hours.** These are planning hypotheses, not measured productivity. Re-estimate after the pilot and evidence/context slice. Sampler redesign and PG14–16 qualification are excluded.

**INFERRED — Do not translate those figures into weeks without measured tokens per round and the owner’s budget.** Keep briefs short, pass artifact manifests rather than full logs, and stop repeated broad consultations. Record actual tokens, rounds and occupied box time after every unit. If early units exceed the budget, reduce or renegotiate scope before opening more work.

**INFERRED — Releasable during migration means each switched feature satisfies its contract and each unswitched feature retains its prior support status.** It does not mean existing defects disappear because old code is still available. Avoid silently blending old and new metrics under indistinguishable labels.

**INFERRED — Old behavior is a migration check for:** trace compatibility, request envelopes, ordering, navigation, formatting and independently validated historical cases. It is **not truth** for DB-Time membership, sample weights, open-tail completeness, category defaults, window placement, percentile populations or summary answers.

**INFERRED — Summaries require a deletion condition, not just parity.** A rollup must state its dimensions, population, coverage and definition version. Use aligned complete interior buckets; answer boundary fragments from raw evidence. If retained raw data is absent and a rollup cannot answer a requested filter/window, report that limitation. Never smear a bucket across an arbitrary boundary and call the result observed.

---

**6. INFERRED — The two separate tracks have interface dependencies**

**INFERRED — Before freezing the evidence contract, the sampler track must decide:**

| Decision | Required measurement | Consequence for the core |
|---|---|---|
| Tick timestamp versus actual observation timing | Compare per-batch/per-backend read brackets against independently bracketed wait transitions; measure ambiguity and delay by connection count. | Support observation brackets/offsets and distinguish unknown legacy timing. |
| Weight and represented support | Jitter, stalls, missed reads and changing populations; check absolute backend-time as well as normalized shares. | Store explicit support/weight and estimator version; never infer all weights from nominal frequency. |
| Selection bias | Repeated phase-randomized short/long workloads under idle and saturated scheduling, with trial/tick dependence respected. | Accurate reads may still produce biased time estimates. Preserve estimated status and an agreed applicability envelope. |
| Attribution coherence | Controlled wait/query/category changes around read brackets. | Values need validity ranges; uncertain attribution goes to explicit unknown/unattributed populations. |
| Sustainable cost | Paired tracer-off/on throughput and latency; total observer CPU/I/O; 1, 50, 200, 500 connections including idle connections. | Determine supported rate/load envelope and whether capture needs redesign. |
| Exact/sample overlap | Controlled escalation, loss and detach boundaries. | Explicit exact-coverage spans, not a broad first-to-last-event mask. |

**VERIFIED — The recorded m8 analysis distinguishes read delay and daemon CPU, and the known-answer driver explicitly warns that its IID error bars are not coverage guarantees under correlated observations.** Neither should be promoted into a general accuracy or application-overhead claim. [m8 output](tests/results/session-20261111/m8/m8-output.txt), [driver](tests/known_answer.py).

**INFERRED — Do not wait for the sampler’s final implementation before writing the pure exact-evidence slice.** But do decide what observations the model must represent before declaring that model frozen. Do not add arbitrary statistical weights to “correct bias” without a defensible sampling design.

**VERIFIED — PG14–16 are not currently just an untested matrix.** Their discovery offsets are omitted; wait-event code describes best-effort table fallback; PG16’s CI cell expects an unsupported-version refusal. [Discovery](src/discovery.c:791), [event tables](src/wait_event.c:499), [CI](.github/workflows/ci.yml:739).

**INFERRED — PG14–16 should not require different reducer arithmetic, but the work is more than capture-side tables.** It includes discovery, activity/query attribution, event dictionaries, phase capabilities and offline interpretation of those capabilities. Persist versioned dictionaries and capability metadata with the capture; normalize them before metrics. The numerical core should not contain `if (pg_major == 16)` branches.

---

**7. INFERRED — Where L1–L5 are wrong**

**VERIFIED — L1 overstates the absence of independent expected answers.** Hand-calculated clipping cases already exist, and the earlier kernel-decomposition harness compares against scheduler evidence. Its provenance is incomplete, but it predates this week. [Clipping tests](tests/test_window_clip.c), [harness provenance](docs/handover-2026-11-10/issue115-harness/README.md).

**INFERRED — L1 is also too monocausal.** Agent workflow can encourage duplicated arithmetic and correlated tests, but evidence loss, ambiguous measurement definitions and platform behavior are technical problems in their own right. The inspected evidence does not establish that the build method caused every defect.

**INFERRED — L2’s file-level keep/discard boundary is wrong.** The middle contains tested arithmetic, ordering and refusal contracts. The proposed keep set contains insufficient stored evidence and unqualified capture behavior. Preserve validated primitives and replace ownership of meaning; do not decide by filename.

**INFERRED — L3 is right about definitions and wrong if it means specifying every future number before any implementation.** Specify the first slice completely, then each subsequent family before its implementation. A comprehensive upfront document can encode misunderstandings just as thoroughly as code. The isolated oracle must be allowed to challenge the definition before it is frozen.

**VERIFIED — L4 describes a benefit that partly exists already.** The repository has a macOS server build script, and several pure computation tests already run without BPF or PostgreSQL. [Local build script](demos/build-server-local.sh).

**INFERRED — L4’s real improvement is dependency separation and CI routing.** Portable tests do not avoid boxes if every `src/` change still schedules the capture matrix. Introduce an explicit dependency boundary and a tested change classifier. Pure arithmetic changes need portable/Linux replay validation; shared writer/capture changes still need appropriate box checks. “Trace in, numbers out” does not make filesystem durability or acquisition pure.

**INFERRED — L5 is operationally useful but architecturally incomplete.** Sampler and PG work can have separate backlogs; their evidence and capability contracts cannot be decided after the core is frozen.

**INFERRED — “Rebuild the middle” is not yet demonstrated cheaper than reworking it in place.** Both approaches must solve the same evidence deficits. A replacement additionally pays for compatibility, integration and reproducing useful optimizations. Its potential advantage is a smaller accounting boundary—not newly written lines.

**INFERRED — The practical choice is incremental extraction with selective replacement.** Put the new public API beside the old code, lift sound pieces, replace tangled reducers behind that boundary, and delete the superseded authority. If the pilot mainly wraps reusable code, that is a successful result. There is no prize for maximizing rewritten lines.

**INFERRED — This plan repeats the last nine months if:** reviewers remain the primary semantic gate; expected values move with implementation; “estimated” becomes a blanket excuse for arbitrary error; scope expands faster than routes are deleted; tests certify only mutually agreeing consumers; or operational acceptance remains an unpriced final phase.

---

**8. INFERRED — The single table the owner sees**

**INFERRED — Use one acceptance ledger, with rows for claims rather than files or PRs.** Each numerical family gets separate rows where evidence mode or platform materially changes the claim. Suggested rows: exact wait time/count; DB Time/AAS; measured and allocated CPU; session/query attribution; executions/percentiles; open-tail behavior; summary boundaries/filters; CLI/UI delivery; sampler accuracy/cost at declared loads; and each supported PG/kernel capability profile.

| Claim and supported scope | Contract | Independent expected answer | Product path | Required faults detected | Provenance | Operational envelope | Old authority retired | Cost / next action |
|---|---|---|---|---|---|---|---|---|
| Example: exact PgSleep time/count, backend generation, arbitrary raw window | Version + example IDs | Literal timeline + independent reference + measured capture where required | Core → server → bridge → visible view | Named mutants and intended failures | Source, binary, input, oracle and environment hashes | Memory/work limit; measured runtime | Specific reducer/path removed or pilot-only | Actual author/implement/review rounds, tokens, box-hours; next blocker |

**INFERRED — A cell turns green only on linked evidence satisfying its own criterion:**

- **Contract:** accepted definition with worked examples and explicit unknown behavior.
- **Independent answer:** frozen derivation or external measurement bounds; product output cannot supply the expectation.
- **Product path:** the actual consumer displays that answer and the specified quality state.
- **Fault detection:** every applicable mandatory mutant fails for its intended reason.
- **Provenance:** the tested binary, inputs and contract are identified; missing provenance cannot be waived by a narrative report.
- **Operational envelope:** the declared size/load/platform is exercised within its resource and accuracy requirements.
- **Retirement:** the obsolete numerical path is removed; an opt-in pilot is visibly still a pilot.

**INFERRED — Refusal correctness and feature support must be separate rows.** PG16’s unsupported-version refusal can be green while PG16 capture support remains red. An unavailable percentile can be correctly presented without making percentile support green.

**INFERRED — A whole claim becomes green only when all required cells are green.** Contract, oracle or relevant implementation changes invalidate the dependent evidence. Completion percentages, passing test counts and old/new agreement do not change its color. This cannot guarantee universal correctness, but it specifically prevents agreement between two wrong implementations from being sufficient acceptance evidence.
