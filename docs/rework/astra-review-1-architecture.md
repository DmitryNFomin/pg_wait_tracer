<!-- Review by the Astra design adviser, 2026-11-11, read-only; reproduced verbatim except that local absolute paths were made repository-relative. -->

**INFERRED — Recommendation: rework the measurement and accounting architecture, but do not rewrite the product wholesale.** The strongest structural problem is that several consumers independently decide what time belongs to which category, window, session, and query. Missing information then travels through inconsistent contracts. Replacing C, Go, or the charting framework would preserve those problems unless those decisions were consolidated.

**INFERRED — One-page decision summary**

| Layer | Verdict | Recommended scope |
|---|---|---|
| Capture: BPF, sampler, daemon, writer, sidecars | **REWORK IN PLACE** | Preserve BPF and platform integration. Rework sampling timing, metadata acquisition, identity lifecycle, and capture-quality reporting. |
| Trace and summary storage | **REWORK IN PLACE** | Preserve the compressed block infrastructure and legacy decoder. Add explicit sample weights, durable open-state evidence, metadata coverage, and reconstructible summaries. |
| Compute and server | **REWORK IN PLACE** | Replace the multiple accounting pipelines with one semantic core. Separate loading, coverage, aggregation, and JSON presentation. |
| CLI replay | **REWORK IN PLACE** | Make replay a consumer of that core. Remove its independent accounting loop. |
| Go bridge and wire protocol | **KEEP bridge; REWORK protocol** | Preserve process supervision, request isolation, and reconnection. Introduce a uniform, typed availability and error contract. |
| Interface | **REWORK IN PLACE** | Preserve views, pure builders, and chart infrastructure. Centralize metric presentation, availability, and lifecycle handling. |

**INFERRED — Planning estimate: approximately 100–160 engineer-days**, including migration, tests, realistic-load validation, and the missing PG14–16 work. This assumes the sampler can be repaired within its current acquisition model. A different sampling mechanism would require a separate investigation and estimate; I would provision an initial additional 15–30 days, without treating that as a guaranteed ceiling.

**VERIFIED — PG13–18 is not merely an unfinished validation matrix.** The current address-discovery table deliberately omits PG14–16, and PG16’s CI smoke test expects an unsupported-version refusal. Their wait-event tables are also unfinished. See [discovery.c:791](src/discovery.c:791), [ci.yml:741](.github/workflows/ci.yml:741), and [ci_smoke.sh:78](tests/ci_smoke.sh:78).

**INFERRED — The three largest risks are:**

1. **Sampling remains biased after correcting weights and timestamps.** The read-first prototype addresses read delay, but does not establish unbiased sampling or solve connection-dependent CPU cost.
2. **Open intervals and irreversible summaries encode different answers.** Consolidating arithmetic alone will not repair missing evidence, frozen classifications, or incorrect placement across window boundaries.
3. **The acceptance machinery can certify the wrong thing.** Stale binaries, vacuous fixtures, mismatched CPU definitions, and platform-inappropriate controls undermine confidence independently of product code.

**INFERRED — What makes defects stop is a reduction in independent semantic decisions, combined with independent oracles.** Another round of helper extraction, more cross-tab comparisons, or a language rewrite would not be enough.

---

**VERIFIED — Review basis.** I reviewed the working tree at `d7f045fd`, with `origin/master` at `e250ff42`. The latter contains the additional phase-capability work for #848; I inspected that difference separately. I read both supplied blocker inventories, fetched the bodies and comments of all 25 open issues through the connected GitHub tool after shell GitHub access failed, inspected the five named branches, and read the relevant source and evidence files. I made no file changes and ran no builds or live capture experiments.

**VERIFIED — Meaning of the labels below.** “VERIFIED” means established from the source, issue discussion, or measurement artifact identified—not that I independently repeated a live experiment. Where I recalculated measurements from raw artifacts, I say so. Recommendations, causal classifications, and estimates are marked “INFERRED.”

**1. Defect classification by structural origin**

**INFERRED — Taxonomy.** Each issue receives one primary classification below to avoid double-counting. Several have secondary causes. Closed-issue classification is less certain: I used the supplied closed titles and inspected relevant implementation paths, but did not fetch every closed issue’s discussion.

**VERIFIED — Inventory arithmetic.** The supplied lists contain **25 open and 51 recently closed issues**. The following assignment contains all 76 exactly once.

| Primary origin — **INFERRED** | Open issues | Recently closed issues | Counts, open / closed |
|---|---|---|---:|
| **A. Acquisition timing, signal sufficiency, and process lifecycle** | #866, #863, #804, #675, #194, #115, #439 | #664, #584 | 7 / 2 |
| **B. Multiple accounting implementations and projections** | #802 | #852, #819, #763, #757, #684, #667, #665, #641, #612, #610, #598, #596, #576, #526 | 1 / 14 |
| **C. Interval identity, window placement, and storage lifecycle** | #855, #853, #427, #410, #381 | #818, #803, #779, #713, #709, #632 | 5 / 6 |
| **D. Missing, partial, or uncertain information loses its meaning** | #848, #839, #837, #808, #341 | #801, #762, #693, #620, #613, #609 | 5 / 6 |
| **E. Test oracle, provenance, and platform-matrix defects** | #865, #861, #832, #760, #622, #573 | #821, #774, #773, #752, #681, #618, #605, #566, #279 | 6 / 9 |
| **F. Local interface lifecycle/recovery bugs** | — | #758, #537 | 0 / 2 |
| **G. Development workflow and governance tooling** | #771 | #719, #717, #714, #710, #706, #705, #704, #703, #702, #701, #691, #687 | 1 / 12 |
| **VERIFIED total** | | | **25 / 51** |

**INFERRED — Architectural significance.**

- **A–D are the product’s structural concentration:** 18 open and 28 closed issues under this assignment. These are not 46 proofs that a rewrite is necessary. They show recurring failures at acquisition, accounting, temporal, and information-quality boundaries.
- **E is a separate architecture problem in verification:** 15 issues. Fixing product architecture without fixing this class leaves the release unverifiable.
- **F contains ordinary localized bugs:** retry and chart lifecycle problems are not reasons to replace the interface framework.
- **G contains 13 development-tooling issues:** they consume time and damage confidence, but provide little evidence for rewriting the tracer.

**VERIFIED — Several issue titles no longer describe the current cause or remaining work accurately:**

- **#381:** the summary producer does have a command-gate classification counterpart. The later discussion identifies window placement as the remaining issue; “missing classifier” is an obsolete diagnosis. The live classification occurs before summary ingestion in [event_stream.c:145](src/event_stream.c:145).
- **#427:** current raw exact occurrence selection is already half-open. Replay still has an inclusive upper-bound check. See [compute.c:142](src/compute.c:142) and [replay_accum.c:128](src/replay_accum.c:128).
- **#410:** the summary `io_worker_busy_pct` absence is now explicitly represented; the AAS/window disagreement remains a separate problem.
- **#573:** the requested `top_events` parity coverage has substantially landed. It should not be estimated as an untouched implementation task.
- **#622:** the latest discussion identifies an inappropriate positive-control test on EL8. Earlier explanations based only on kernel capability were corrected in the issue itself.
- **#675:** the comparator’s “CPU” includes no-wait wall time. Its historical ratio is not a calibrated measurement of scheduler CPU error. The later conservation experiment materially changes the diagnosis. See [#675 discussion](https://github.com/DmitryNFomin/pg_wait_tracer/issues/675).

**INFERRED — Therefore the open count is neither a count of independent root causes nor a reliable remaining-work estimate.** The retrospective’s warnings about unverified comparisons and stale diagnoses are relevant to this review, not merely historical process criticism.

**2. What the source measurements show**

**VERIFIED — Size inventory.** These are physical lines and bytes, including comments—not executable lines. The tracked-source inventory, excluding `vendor`, `results`, and `node_modules`, contains 64,502 lines under `src/`, 19,724 under `web/`, and 175,792 under `tests/` for the inspected source/script extensions.

| File | Lines | Bytes |
|---|---:|---:|
| `src/server.c` | 9,267 | 426,638 |
| `src/compute.c` | 6,346 | 279,638 |
| `src/summary_writer.c` | 2,751 | 128,718 |
| `src/summary_writer.h` | 1,418 | 84,399 |
| `src/backend_status_layout.c` | 2,709 | 107,517 |
| `src/daemon.c` | 2,013 | 92,802 |
| `src/event_writer.c` | 1,813 | 73,612 |
| `src/sampler.c` | 1,532 | 68,456 |
| `src/bpf/pg_wait_tracer.bpf.c` | 1,166 | 48,668 |
| `src/event_reader.c` | 749 | 31,103 |
| `src/replay_accum.c` | 446 | 22,202 |
| `web/bridge.go` | 665 | 20,734 |
| `web/static/app.js` | 1,684 | 74,757 |
| `web/static/lib/builders/table-configs.js` | 922 | 50,131 |

**INFERRED — Size alone does not justify replacement.** More significant is that `server.c` combines storage discovery, caching, coverage, routing, metadata interpretation, analytical orchestration, and JSON presentation, while `compute.c` contains parallel raw and summary accounting families.

**VERIFIED — Accounting ownership inventory.** I counted semantic aggregation sites, not textual occurrences of arithmetic operators. Shared low-level helpers do not make two independent accumulation loops one implementation. Conversely, different populations—sample observations, wait occurrences, and executions—must not be counted as interchangeable implementations of “count.”

| Quantity | Independently maintained sites in the inspected core |
|---|---|
| **DB Time / active-time totals** | **4 raw reducers:** Time Model, Events, Sessions, Queries; **1 summary materializer plus 4 summary readers** for those projections; **1 live accumulator and 1 replay accumulator**. Thus **11 aggregation sites**, including the currently unused summary Queries reader. Queries also separates attributed and unattributed populations. |
| **CPU / off-CPU split** | **4 raw paths:** AAS, Time Model, Events, Sessions; the summary materializer and **4 corresponding summary read paths**; live and replay accumulation. Shared clipping helpers reduce arithmetic duplication, but classification, admission, and accumulation remain separate. |
| **Total idle time** | Raw Time Model, summary materializer, summary Time Model reader, live accumulator, replay accumulator: **5 core sites**. Other reducers independently decide whether idle records are admitted. |
| **Full per-event totals** | Raw Time Model and Events; summary materializer and their two readers; live and replay: **7 sites**. AAS event bands and session/query top-event projections add separate dimensional aggregations. |
| **AAS** | **3 raw calculation families:** chart buckets, Time Model rows, Events rows; **3 summary counterparts**. The UI additionally constructs the synthetic off-CPU presentation row. |
| **Event occurrence counts** | Raw Events, summary materializer, summary Events reader, live, replay: **5 sites**. Query-record counts have an analogous set. Transition, execution, plan, and variant counts are additional, distinct populations. |
| **Percentiles** | **3 policies:** histogram-derived event bounds shared by raw/summary Events; execution/plan percentile selection in `percentile.h`; variant percentile selection in `compute.c`. Separately, the latency-to-histogram-bin mapping exists in **3 places**: compute, map reader, summary writer. |
| **Per-session totals** | Raw reducer, summary materializer, summary reader, live accumulator, replay accumulator: **5 sites**. |
| **Per-query totals** | Raw reducer, summary materializer, summary reader, live attribution/accumulation, replay accumulation: **5 sites**, with some shared attribution machinery. The summary query reader is not the server’s current production route. |

**VERIFIED — Locations behind that inventory:** raw reducers begin at `compute.c:909`, `1448`, `1887`, `2122`, `2336`, and `2542`; the six summary counterparts occupy approximately `2690–3880`. Live accounting starts at [map_reader.c:121](src/map_reader.c:121), replay at [replay_accum.c:47](src/replay_accum.c:47), and summary materialization at [summary_writer.c:323](src/summary_writer.c:323). Five summary routes are dispatched by the server; Queries explicitly uses raw at [server.c:6025](src/server.c:6025).

**VERIFIED — Cross-validation is another semantic implementation.** `tests/cross_validate.c`, 903 lines, has its own command-gate and sample/exact accounting. It explicitly says its CPU comparator means `we==0` **wall time**, not `cpu_ns`, at [cross_validate.c:129](tests/cross_validate.c:129). Its use of the shared trace reader does not make its numerical interpretation an independent scheduler-CPU oracle.

**VERIFIED — The interface does not independently reconstruct all those metrics.** Much of it formats server values. However, `addCpuWaitingRow` synthesizes an accounting row and calculates its DB-Time percentage at [table-configs.js:498](web/static/lib/builders/table-configs.js:498). Compare-mode omission of that row produced #757. This is a specific downstream accounting responsibility, not evidence that all frontend code recomputes DB Time.

**VERIFIED — These counts are a scoped inventory, not an exhaustive count of every analytical loop.** Transitions, fingerprints, concurrency, lock chains, interference, execution detail, lifecycle, and variants contain additional logic. They strengthen the need for shared input semantics, but their different analytical outputs should not be falsely described as copies of DB-Time computation.

**VERIFIED — Trace decoding is more centralized than accounting:**

- There is **one production trace-payload decoder**, in `event_reader.c`, used by server, replay, and cross-validation.
- Trace container/header interpretation occurs in **three production files**: reader, writer recovery, and server coverage diagnosis.
- Summary payload deserialization has **one implementation**, presently located in `summary_writer.c`, called by `summary_reader.c`; summary reader and writer recovery separately interpret their container.
- I found **two Python trace-format readers**: `tests/replay_ui.py` reads the file header; `tests/test_data_current_trace_cache.py` interprets headers, blocks, footer, and index.
- `tests/open_ckpt_stub.c` also reads the trace header. C format tests directly inspect headers/blocks, and `test_data_sessions.py` inspects a summary header. These are test-format dependencies, not additional full analytical decoders.
- Neither Go nor browser builders decode trace payloads.

**INFERRED — Consequently, “rewrite all the parsers” would target the wrong concentration of duplication.** The shared decoder should be retained and its container/coverage API strengthened.

**VERIFIED — Current representations of missing information differ by boundary:**

| Boundary | Current representation and consequence |
|---|---|
| Capture/trace CPU | `PGWT_CPU_NS_UNKNOWN == UINT64_MAX`; measured zero is distinct. |
| Backend category | Lookup can return `0`, also the foreground/default category. Missing metadata can therefore become a plausible classification. |
| Query attribution | `query_id == 0` plus flags and context distinguish several unattributed cases. Zero alone is insufficient. |
| Compute results | Mixture of booleans, sentinels, result flags, absent fields, and early returns. Some allocation failures return a zero-initialized result rather than an explicit failure. |
| Percentages | `PGWT_PCT_DB_IDLE == -1`, subsequently translated to null/dash presentation. |
| CPU quality | `has_measured_cpu` can mean **at least one** measured gap, not complete measured coverage. |
| JSON | Null, omission, `*_unavailable`, reason strings, fidelity labels/ranges, and command errors coexist. |
| Bridge | Structured request errors coexist with refusal reasons inferred from stderr text. |
| UI | Each builder/view must interpret the applicable subset correctly. |

**VERIFIED — Examples:** [pg_wait_tracer.h:215](src/pg_wait_tracer.h:215), [compute.c:402](src/compute.c:402), [compute.h:463](src/compute.h:463), and the allocation-failure return at [compute.c:929](src/compute.c:929).

**INFERRED — This is the second major structural defect after accounting duplication:** consumers need undocumented combinations of fields to distinguish zero, unknown, incomplete, unsupported, and failed.

**2a. Capture — REWORK IN PLACE**

**VERIFIED — Today’s scaling evidence survives recalculation.** I opened the archived raw `cpumon.jsonl`, `window.json`, and binary read-bracket records inside `m89-raw.tar.gz`, without extracting or modifying files. For the 60-second windows, I independently reproduced:

| Extra idle connections | Sampled processes | Daemon CPU per tick | CPU share of one core | Median timestamp-to-read delay |
|---:|---:|---:|---:|---:|
| 0 | 12 | 0.634 ms | 0.633% | 208 µs |
| 50 | 62 | 1.235 ms | 1.233% | 779 µs |
| 200 | 212 | 3.194 ms | 3.183% | 2,467 µs |
| 500 | 512 | 6.861 ms | 6.850% | 5,777 µs |
| 0, repeated | 12 | 0.635 ms | 0.633% | 212 µs |
| Read-first prototype, 500 | 512 | 6.945 ms | 6.933% | 0.812 µs |

**VERIFIED — Measurement scope:** Ubuntu 6.8, PG18.6, 10 Hz, four active subjects plus idle connections, one repetition per scaling cell, an instrumented build based on `d7f045fd`. CPU counters have clock-tick quantization and approximately half-second observation spacing. This measures daemon CPU, not PostgreSQL throughput or latency impact. Evidence: [m8 output](tests/results/session-20261111/m8/m8-output.txt), [m8 analysis](tests/results/session-20261111/m8/m8.py), and [#866](https://github.com/DmitryNFomin/pg_wait_tracer/issues/866).

**VERIFIED — The current ordering explains why the timestamp can precede the read substantially.** The sampler performs per-target work before the batched wait-word read. The PG backend-status path opens `/proc/<pid>/mem`, reads attribution state, and closes it per call at [backend_status_layout.c:1353](src/backend_status_layout.c:1353).

**INFERRED — That is a strong profiling target, not proof of the dominant CPU cost.** The measurements do not yet justify a specific cache strategy or an exact promised saving.

**VERIFIED — The sample-weight defect is independent of that delay.** `pgwt_sampler_effective_period` raises intervals shorter than nominal to nominal, while the timer deliberately jitters around nominal. See [sampler.c:538](src/sampler.c:538). The known-answer experiments report 61.2–61.7 seconds of sampled DB Time in a 60-second backend window; share-based comparisons can conceal a common scale error. See [#863](https://github.com/DmitryNFomin/pg_wait_tracer/issues/863).

**VERIFIED — The read-first results do not close sampling correctness.** The recorded short-cycle errors remain workload- and scheduling-dependent; the 500-idle prototype cell reports approximately +6.2 percentage points. Its wider read bracket also excludes many observations from the unambiguous instantaneous-read comparison. Zero mismatches among the remaining observations does not establish an unbiased time estimate.

**INFERRED — Specific rework:**

- Read prepared wait-word targets promptly, with timestamps describing the actual observation.
- Separate cheap observation from expensive enrichment.
- Cache immutable identity/layout information by postmaster and backend generation, with explicit invalidation.
- Preserve attribution consistency checks; stale metadata must not silently attach a new query identity to an earlier wait observation.
- Store actual weighting information and explicit uncovered intervals after stalls.
- Measure daemon cost and PostgreSQL impact at hundreds of connections and under churn before choosing further optimization.

**VERIFIED — What a wholesale rewrite would discard:** working watchpoint/bootstrap integration, scheduler accounting, process lifecycle handling, discovery/runtime validation, exact-tier escalation, bounded capture mechanisms, and substantial platform-specific tests. The later #675 experiment found the EL8 state-map CPU near the independent scheduler reference, although residuals and map-to-trace differences remain unresolved.

**INFERRED — That evidence supports keeping the BPF foundation while investigating acquisition and downstream attribution.** It does not support replacing BPF because a comparator reports a large CPU discrepancy.

**2b. Trace and summary storage — REWORK IN PLACE**

**VERIFIED — Raw trace v3 stores transition columns including timestamp, PID, events, duration, query ID, and CPU. Sample blocks carry point observations with a block sample period. Backend-category context is not preserved as a complete self-contained classification in each decoded record.** See [event_writer.h:24](src/event_writer.h:24) and [event_reader.c:260](src/event_reader.c:260).

**VERIFIED — Summary format version 12 is an accounting format, not merely compression.** It materializes classifications, counts, histograms, session/query totals, and open credits. Current computation rejects old accounting versions and falls back to raw. Its bounded tables include 1,024 events, 1,024 sessions, and 2,048 queries per record. See [summary_writer.h:544](src/summary_writer.h:544) and [summary_reader.c:38](src/summary_reader.c:38).

**VERIFIED — Open-state support is substantially implemented but not integrated end to end.** `pgwt_overlay_attach` exists in a 1,176-line module, but there is no server call. `pgwt_writer_checkpoint_open` likewise has no production caller. Meanwhile the raw loader uses finite right lookahead, which cannot find a closing record that does not yet exist. See [open_overlay.c:1085](src/open_overlay.c:1085), [event_writer.c:471](src/event_writer.c:471), and [server.c:1713](src/server.c:1713).

**VERIFIED — Sidecar integrity matters numerically.** The backend metadata writer does not check the `fprintf` and `fflush` results, and its records identify a PID without an explicit validity interval/backend generation. See [backend_meta.c:23](src/backend_meta.c:23). #839 demonstrates incorrect totals when category metadata cannot be read.

**INFERRED — The structural repair is to make evidence and its coverage durable, then make summaries disposable derivatives.** Merely raising the 300-second scan limit moves the failure boundary. Merely sharing arithmetic leaves the raw route without evidence for an open interval.

**INFERRED — Keep** compression, block indexing, committed-watermark handling, rotation/recovery protections, and legacy decoding. **Replace** the independent summary accounting policy and add explicit interval identity, metadata coverage, and open-state representation. PID-reuse ambiguity is a further design risk inferred from the metadata format; I did not establish a live PID-reuse defect in this review.

**2c. Compute and server — REWORK IN PLACE**

**VERIFIED — The server already contains useful central mechanisms:** shared CPU clipping, centralized portions of load refusal, cache limits, raw/summary eligibility checks, and current-version summary preflight. `window_clip.h` uses cumulative integer allocation so adjacent window contributions add consistently.

**VERIFIED — Those mechanisms do not establish one accounting implementation.** For example, raw AAS apportions CPU with floating-point overlap arithmetic, while table reducers use the shared integer helper. Live command classification is explicitly described as a “twin” that must stay synchronized with `pgwt_tag_events`. Summary open-credit classification can be frozen before the closing record arrives.

**INFERRED — Replace the semantic core, not the whole server.** A server-file split without moving policy ownership would make the code easier to navigate but leave the defect mechanism intact.

**VERIFIED — There is also complexity that supplies no current production benefit.** `bagg_window_eligible` unconditionally returns zero because its untagged records do not preserve command-state semantics. See [server.c:7794](src/server.c:7794).

**INFERRED — A rewrite would unnecessarily discard mature loading, request handling, refusal behavior, and analytical features.** The useful replacement boundary is between decoded evidence and metric results.

**2d. CLI replay — REWORK IN PLACE**

**VERIFIED — Replay is not merely a formatter.** Its 446-line accumulator independently selects windows, counts occurrences, classifies records, splits CPU, and accumulates event/session/query totals. It shares several helpers, but has separate orchestration and incomplete metadata integration.

**VERIFIED — The corrected #802 discussion narrows the original headline.** On one matched sampled window, replay and server DB Time agree while Idle differs; on a full trace, much of the DB-Time difference is explained by command gating. Category metadata remains a concrete missing dependency. See [#802](https://github.com/DmitryNFomin/pg_wait_tracer/issues/802).

**INFERRED — Make CLI replay call the same query/metric library as the server.** Preserve command-line behavior, streaming file access, formatting, and diagnostics. Delete the independent replay accounting policy after migration.

**2e. Go bridge and protocol — KEEP / REWORK**

**VERIFIED — The bridge is relatively small and already implements valuable isolation and recovery behavior.** Browser transport rejects error responses, supersedes requests by channel, drops late responses, and rejects pending work on disconnect. See [transport.js:30](web/static/lib/transport.js:30).

**VERIFIED — Refusal reporting still parses stderr vocabulary.** `web/refusal.go` distinguishes `FATAL`, `REFUSED`, and a special unmarked string. Its comments explain that a source-text mutation test helped preserve this interim workaround. See [refusal.go:31](web/refusal.go:31).

**INFERRED — Keep Go and the bridge process model.** Replace text-derived operational meaning with structured startup/request errors and versioned capabilities. There is no evidence here that moving the compute engine into Go, or removing Go, would materially reduce the identified numerical defects.

**2f. Interface — REWORK IN PLACE**

**VERIFIED — The existing interface already has 12 view modules and 12 builder modules, plus transport and view-management infrastructure.** Pure builders and browser tests are valuable existing assets. The problem is not the absence of a frontend architecture.

**VERIFIED — Quality propagation remains distributed.** On the reviewed checkout, Waterfall consumes the right-edge incompleteness information while other displayed quantities need separate fixes. The #855 branch touches 38 files to propagate and present that condition.

**INFERRED — Introduce a single response-to-view-model boundary and common metric components.** Every view should receive already-interpreted value, unit, population, basis, completeness, and reason information. Views should choose layout, not invent whether a number is trustworthy.

**INFERRED — Preserve** navigation, drill-downs, selection, chart integration, pure builders, and useful tests. **Remove** metric synthesis and ad hoc availability interpretation from individual panels. Ordinary mounting/retry defects still require lifecycle tests; a new framework would not automatically eliminate them.

**VERIFIED — The unmerged branches show the current breadth of semantic fixes.** These are three-dot diffs from the merge base with `origin/master`, including tests and fixtures:

| Branch | Files | Insertions / deletions |
|---|---:|---:|
| `agent/863-sample-weight-sum` | 53 | +1,663 / −298 |
| `agent/839-category-reader` | 27 | +1,997 / −224 |
| `agent/855-right-edge-incomplete-ui` | 38 | +1,420 / −148 |
| `agent/861-box-check-stale-build` | 12 | +1,042 / −122 |
| `agent/115-read-first-proto` | 40 | +1,882 / −380 |

**VERIFIED — The prototype total includes ancestor work.** Its own commit, `544486d4`, changes three files, +474/−122. It would be misleading to call all 40 files the cost of moving the read earlier.

**INFERRED — These diffs demonstrate broad contracts, not automatically wasted work.** Much of the test expansion is appropriate. The architectural goal is for the next semantic correction to have one implementation change and broad consumer verification.

**3. Concrete target design and migration**

**INFERRED — Use a shared C library with five explicit responsibilities.** This avoids a language migration while changing the ownership of meaning.

| Proposed module | Sole responsibility |
|---|---|
| `trace_store` | Decode supported formats; enumerate committed evidence; return typed coverage, corruption, version, and resource-limit results. Own container interpretation used by reader, recovery, and diagnostics. |
| `capture_context` | Resolve backend identity/generation, category, query identity, capabilities, and their validity ranges. Never default missing category to foreground. |
| `interval_model` | Convert evidence into canonical intervals, weighted samples, markers, and open-state updates. Own classification, attribution, clipping, and occurrence admission. |
| `metrics` | Produce contribution vectors and aggregate them into DB Time, CPU/off-CPU, idle, event, session, query, histogram, count, and AAS results. |
| `query_service` | Select raw/rollup inputs, apply supported filters, enforce budgets, and expose results to CLI and JSON adapters. |

**INFERRED — Compute each quantity in one place as follows:**

- **DB Time, idle, CPU/off-CPU, and event time:** one contribution function over canonical evidence.
- **Session and query totals:** group those same contributions; preserve explicit unattributed buckets.
- **AAS:** divide the same bucket contributions by the declared bucket exposure.
- **Counts:** one admission function for each named population—wait occurrences, samples, transitions, executions, plans. Do not overload one `count` field.
- **Percentiles:** one policy per named population and representation, with explicit histogram bounds or sample-size semantics.
- **Percentages and compare deltas:** a shared result/presentation adapter with declared denominators.
- **Summary generation:** aggregate those same contributions; do not reclassify the evidence.

**INFERRED — Keep two accounting axes distinct.** PostgreSQL wait state and scheduler CPU state overlap: a backend can consume CPU while reporting a wait. An exclusive DB-Time partition and a diagnostic of total scheduled CPU are different quantities. The design must specify where protocol-tail CPU belongs for #194, and must not add wait-spin CPU to wait-wall time twice. This specification needs executable examples, not just field names.

**INFERRED — Window semantics must also distinguish observation from allocation.** The shared integer clipping helper can conserve a measured interval’s CPU total across windows. It cannot prove when within that interval the CPU ran. Results produced by proportional allocation should declare that basis rather than claim direct sub-window measurement.

**INFERRED — Represent unknown end to end using separate dimensions:**

```text
Metric:
  value: optional
  unit
  population
  basis: observed | sampled_estimate | allocated | histogram_bound
  completeness: complete | partial | unavailable
  reasons[]
  coverage
```

**INFERRED — Rules for that contract:**

- Measured zero has a value of zero; unavailable has no value.
- Missing metadata invalidates dependent metrics, while independent measurements can remain usable.
- A lower bound is emitted only where mathematically justified.
- Missing positive time may make a total a lower bound; it does **not** generally make a percentage or percentile a lower bound.
- Capability belongs to the capture and time range, not the host currently opening the trace.
- Resource exhaustion, unsupported data, and corrupt committed evidence produce structured refusals, never empty-looking success.

**INFERRED — Store enough evidence to remove future lookahead as a correctness requirement.** Persist interval starts/state changes and committed open-state checkpoints with stable identity and validity watermarks. Closing records reconcile against that identity exactly once. Readers must not extrapolate beyond known capture coverage.

**INFERRED — Make summaries reconstructible rollups.** Use complete aligned interior buckets; obtain partial boundary buckets from canonical raw evidence. Store sufficient statistics for supported filters. If a rollup lacks the required dimensions or population, use raw or return unavailable. A top-N list must never masquerade as a complete aggregate.

**INFERRED — Keep summary work off the acquisition-critical path where practical.** First share the reducer within existing processes. Then allow bounded asynchronous materialization from committed trace data, potentially off the database host. Moving work into another process alone does not reduce total overhead; CPU and I/O budgets still need measurement.

**INFERRED — Delete, after replacement is proven:**

- Independent replay accumulation policy.
- Raw/summary copies of classification and CPU/idle admission.
- Duplicate histogram-bin definitions.
- Unused summary-query and disabled block-aggregate production paths.
- Frontend construction of accounting rows.
- Stderr-text interpretation as the normal machine-readable error channel.
- Old implementations retained only to serve as permanent parity oracles.

**INFERRED — Migration order and estimate.** “Shippable” here means each step can land without knowingly degrading the supported behavior. It does not mean today’s tree already meets the owner’s release definition.

| Step | Work | What remains usable | Engineer-days |
|---|---|---|---:|
| 1 | Bind evidence to binaries; establish independent numerical fixtures and capability matrix | Existing entry points and formats | 8–12 |
| 2 | Repair sample weights; profile and rework read/enrichment ordering; validate observation quality | Existing capture modes, with explicit limits | 12–20 |
| 3 | Introduce canonical context, interval, and metric core; migrate raw reducers incrementally | Old wire responses through adapters | 18–28 |
| 4 | Integrate durable open state and metadata coverage; replace summary accounting with rollups | Raw remains the fallback; incompatible rollups refused | 18–30 |
| 5 | Move CLI replay to the shared query service | Existing CLI presentation | 6–10 |
| 6 | Introduce typed wire quality and common UI view models | Existing views migrated individually | 10–16 |
| 7 | Complete PG14–16 adapters/tables and actual capture support | Existing PG13/17/18 paths retained | 15–25 |
| 8 | Run end-to-end platform, churn, failure, and overhead qualification | Release candidate with explicit capability behavior | 10–15 |
| **INFERRED total** | | | **97–156; plan as 100–160** |

**INFERRED — Do not automatically dual-run all accounting in production.** Shadow comparisons belong in tests, replay tools, and opt-in diagnostics; doubling work by default conflicts with the owner’s overhead priority.

**4. Smallest structural set and the test oracle**

**INFERRED — The minimum durable set is five changes:** shared accounting; durable temporal/metadata coverage; typed quality/error results; independent numerical oracles; and trustworthy build/platform provenance. A rewrite without these five would reproduce the same defect classes.

| Class | Structural prevention — **INFERRED** | Specific automatic oracle — **INFERRED** |
|---|---|---|
| **A. Acquisition/lifecycle** | Timestamp actual observations; conserve explicit sample weights; key caches by process generation; separate observation from enrichment | Known-answer PostgreSQL worker with independently bracketed wait-word changes; controlled jitter/stall sequences; paired daemon-on/off throughput and latency at 300–500 connections; churn and reattach tests. Compare CPU against scheduler CPU with identical PID/window scope. |
| **B. Competing accounting** | One contribution function and one query service consumed by server, replay, and rollups | Hand-calculated small timelines with exact expected integer results. Assert system = sessions = attributed queries + unattributed contributions under identical scope. Mutate a result to zero and require failure even if every route agrees. |
| **C. Temporal/storage** | Stable interval identity; open-state evidence; half-open occurrence semantics; raw boundary handling | Split/concatenate-window properties; long waits crossing arbitrary boundaries; duplicate/reordered records; crash at commit boundaries; restart, rotation, retention, and PID reuse. Check both time placement and event identity, not only total conservation. |
| **D. Information quality** | Typed metric state and shared UI presentation | Generate every availability state through the real server → bridge → browser path. Remove/corrupt sidecars, truncate evidence, exceed top-N/capacity, and mix capabilities. Assert unavailable is never rendered as zero or “no activity.” |
| **E. Verification** | Build identity tied to source tree, toolchain, and actual executable; explicit capability matrix | Deliberately supply a stale executable, wrong kernel/PG identity, missing workload, unavailable positive control, and fabricated output. Each must produce the intended failing or capability-refusal result. |
| **F. UI lifecycle** | Central mount/unmount and request-generation ownership | Browser tests with delayed/reordered replies, rapid navigation, disconnect/reconnect, timeout/retry, and resize. Assert final data ownership, visible state, chart disposal, and no uncaught errors. |
| **G. Workflow tooling** | Structured state and actual operation inspection instead of prose/string inference | Fixture-driven hook/label/runner tests, including malformed reports, read-only commands, alternative push paths, and explicit state transitions. Keep these separate from product correctness. |

**VERIFIED — Some building blocks already exist:** pure builders, transport cancellation, mutation tests, golden-format fixtures, shared clipping helpers, and the known-answer workload work. The proposal consolidates and strengthens them rather than discarding the test suite.

**INFERRED — Two oracle rules are essential:**

1. **Parity is a migration check, not truth.** Two routes can agree on fabricated zeros, wrong membership, or wrong time placement.
2. **Conservation is necessary but insufficient.** The #818 defect moved time to the wrong event while preserving the pair total; the #863 defect can disappear in ratios.

**INFERRED — No finite suite makes all acquisition bias or UI glitches impossible.** Shared ownership can make *route-specific policy drift* impossible by construction. Independent fixtures and fault injection can catch the enumerated mechanisms automatically. Statistical sampling behavior and unfamiliar platform behavior still require continuing measurement.

**5. What I would disable or cut for the release**

**INFERRED — These are explicit scope recommendations, not permission to silently omit existing promises.** Under the owner’s strict “every feature already added works” definition, a permanent feature cut requires an explicit release-scope decision. Platform capabilities that cannot exist can legitimately remain unavailable with a reason.

| Proposed cut or disablement — **INFERRED** | Stated reason | What it saves |
|---|---|---|
| **Disable the summary route until its semantics match the canonical core** | Current raw/summary differences affect numbers, not only speed | Removes one active numerical authority during migration. If omitted for the entire release, approximately **15–25 engineer-days** of rollup work could be deferred; long-window latency and answerability worsen. |
| **Remove the already-disabled block-aggregate production fast path and unused summary Queries route** | They add maintenance surface without current user benefit | Reduces future semantic migration and validation work; no current runtime speed is lost from the unconditionally disabled path. |
| **Disable phase-dependent execution/plan features when capture capability is unavailable or unknown** | “No executions” is an incorrect substitute for unavailable instrumentation | Avoids misleading empty Waterfall/Scatter/statistics results and invalid platform controls. This does not remove ordinary wait analysis on EL8. |
| **Disable event-level AAS breakdown until it has an explicit remainder and correct totals** | A top-16 stack is not a complete decomposition | Temporarily removes #837’s misleading visualization and its release-validation surface. Class-level AAS remains available. |
| **Do not release unqualified high-rate sampled modes before accuracy and cost validation** | Existing failures are not fixed by more samples or broader tolerances | Reduces rate/platform/load combinations requiring qualification. It does not establish that 10 Hz is already acceptable. |
| **Do not emit latency percentiles from unavailable or incomplete populations** | A plausible percentile is worse than a stated absence | Removes unsupported numerical claims; preserve counts/time where independently justified. |

**INFERRED — I would not cut CLI replay, replace the UI framework, or remove the Go bridge to save this release.** Their reusable infrastructure is valuable, and replay is an important second consumer for testing the common engine.

**INFERRED — I would not count “PG14–16 refuse cleanly” as satisfying PG13–18 support.** The owner’s compatibility requirement makes that implementation work mandatory.

**INFERRED — I would also reject an unrestricted “raw-only” fallback.** It needs bounded memory and work, with an explicit “window exceeds available processing budget” result. Disabling summaries does not authorize hangs, crashes, or silently smaller windows.

**6. Where this analysis is weakest**

**VERIFIED — I did not execute the product or qualification suites.** I did not attach BPF on EL8, run PG14–16, measure PostgreSQL throughput/latency, or exercise every UI interaction. Source and existing artifacts establish substantial problems and useful boundaries; they do not establish a passing release.

**VERIFIED — I independently recalculated the m8 scaling CPU/read-delay figures and the m9 500-connection CPU/read-delay figures.** I read the known-answer and prototype analysis scripts/results, but did not independently reconstruct every truth interval and every statistical result from all experiment archives.

**VERIFIED — I did not audit every source line.** I concentrated on capture ordering, decoding, classification, window accounting, summary production/consumption, replay, transport, builders, and relevant tests. I read the working agreement, handover, retrospective, and current/relevant roadmap sections; I did not line-audit the roadmap’s entire historical and future-feature archive.

**VERIFIED — The closed-issue taxonomy is weaker than the open-issue taxonomy.** Closed issue bodies/comments were not all fetched. Some closed items could move between accounting, information-quality, and verification classes after that investigation.

**INFERRED — The largest unresolved technical question is whether low-overhead sampling can meet the required accuracy envelope at realistic concurrency on EL8.** Read-first improves observation timing, but selection bias, read-bracket width, attribution coherence, and total cost remain distinct concerns.

**INFERRED — Evidence that would change my verdict:**

- If controlled experiments show the current userspace sampling mechanism cannot meet the agreed accuracy/overhead envelope even after enrichment is removed from the critical path, I would recommend **replacing that acquisition mechanism**.
- If existing open-state evidence can be integrated and the same canonical reducer can generate summaries without revisable classifications, the storage estimate could shrink materially.
- If PG14–16 require substantially different discovery or attribution mechanisms across supported builds, the compatibility estimate would grow.
- If a complete UI run finds systemic lifecycle failures outside the current view manager, frontend restructuring may need to be broader—but the present evidence does not justify replacing the rendering stack.

**INFERRED — The fundamental change is that capture records evidence, one engine assigns meaning, and every consumer preserves the engine’s uncertainty.** That is the architectural boundary currently missing, and it is the change most likely to turn repeated numerical defects into bounded implementation work.
