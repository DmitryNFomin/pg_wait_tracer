
**Single fixture:** add `refresh-near-ties` beside [table.test.mjs:74](tests/web_unit/table.test.mjs:74): shuffled identical input, alternating 100/101 values, then 100/106, selected-row deletion and explicit re-sort. Expect no jitter, then a material reorder, with identity preserved. **Negative control:** replace the policy with exact descending sorting every tick; the alternating sequence must fail.
tokens used
102,672
These are proposed contracts to insert at [RELEASE-PLAN.md:118](tests/results/RELEASE-PLAN.md:118). Existing behavior and required changes are distinguished below. The fixtures and negative controls are acceptance requirements, not claims that they have already run.

1. **Population and denominators — #435, #425, #439**

**Contract:** Preserve the decided model: **total DB Time = foreground + maintenance + background active time**. Foreground comprises client work and attributed parallel-worker work, including planning, execution and command overhead. Idle and io_workers are excluded. Client CPU is command-gated; background CPU is not. Pacing sleeps are idle; `PgSleep` remains active. These rules already exist at [AAS_SEMANTICS_DECISION.md:10](docs/AAS_SEMANTICS_DECISION.md:10), [:43](docs/AAS_SEMANTICS_DECISION.md:43) and [:171](docs/AAS_SEMANTICS_DECISION.md:171).

- Overview, AAS and Events use **all qualifying active processes** after filters. `%DB` divides by their filtered total DB Time; AAS divides by elapsed window time, not session count.
- Sessions includes foreground, maintenance and background processes. Each row’s CPU/Wait percentages divide by **that PID’s** active time ([compute.c:1571](src/compute.c:1571)).
- Queries currently divides by **query-attributed active time**, excluding query-id-zero work ([compute.c:1691](src/compute.c:1691)). Label its percentage “% attributed DB Time”; disclose unattributed foreground separately. It is not the Overview denominator.
- Idle share is **not applicable**: render `—` with “Excluded from DB Time”, never meaningful `0%`. Its existing time/window rate may be labelled **idle occupancy**, never active-session AAS. For #425’s unverified paths, idle duration and occupancy are **unavailable** until independently verified.

**Already correct:** background work belonging to total DB Time; #439 can therefore be population labelling, not exclusion. **Contradiction:** Overview serializes idle’s numeric zero at [server.c:3533](src/server.c:3533); Events correctly emits null at [:3661](src/server.c:3661). Record the presentation rule beside [table-configs.js:108](web/static/lib/builders/table-configs.js:108).

**Single fixture:** extend [test_data_categories.py:39](tests/test_data_categories.py:39) into `population-contract`: 5 seconds each of foreground, maintenance, background and io_worker work, plus known idle. Expect total DB Time 15 seconds, foreground 5 seconds, io_worker excluded, idle shares `—`. **Negative control:** charge the worker’s 5 seconds to DB Time or render idle share as zero; the same checker must reject either.

2. **Window contribution versus latency — #426**

**Contract:** For interval `[s,e)` and window `[a,b)`, contribution is `max(0, min(e,b) − max(s,a))`. Overview totals, Events Total/%DB/AAS, Sessions DB Time, Queries DB Time/profile and **AAS top-N membership** use that contribution. Latency statistics use full durations of qualifying exact records; Histogram uses full wait latency, and execution latency uses marker-pair duration.

A view showing both must label them **“Time in window”** and **“Full-event latency”**. It must not imply `window total = count × full-event average`. An explicit latency sort ranks the displayed latency/bound; default DB-Time ranking uses contribution. Missing full-duration evidence means **unavailable**, not contribution/count.

**Already correct:** Events explicitly separates these accumulators at [compute.c:1443](src/compute.c:1443); record the shared rule there. **Contradiction:** AAS plots bucket overlap at [:605](src/compute.c:605), but selects top events using full duration at [:639](src/compute.c:639). Fix membership, not the already stable stack ordering. This contract does not claim that the loader already supplies every open or trailing-boundary interval.

**Single fixture:** add `rank-reversing-boundary` to [test_window_clip.c:179](tests/test_window_clip.c:179): A has 10 seconds full duration/1 second contribution; B has 2 seconds/2 seconds. With top-N=1, B wins; A’s latency remains 10 seconds. **Negative control:** restore full-duration accumulation at line 639; membership must fail.

3. **Wait counts, execution counts and averages — #441**

**Contract:** An execution count counts completed, matched `EXEC_START/EXEC_END` pairs; average execution time is their summed full durations/count. It is neither command duration nor accumulated DB Time divided by executions. Missing pairs mean **unavailable**, not zero executions inferred from waits.

There is an important naming defect: Queries’ existing `count` counts **non-idle wait/CPU records**, including CPU intervals; sampled continuations do not add observations. It is not a pure wait count ([compute.c:1713](src/compute.c:1713)). Preserve that metric as “Wait/CPU intervals” for exact data and “Observations” for sampled data. Its exact-only full-duration average may be labelled “Avg interval”; a pure **Avg Wait is unavailable** from this aggregate. Do not invent it by dividing DB Time by another count.

**Already correct:** lifecycle averaging at [server.c:3903](src/server.c:3903), and separate execution/interval columns at [table-configs.js:225](web/static/lib/builders/table-configs.js:225). Record corrected names at [:244](web/static/lib/builders/table-configs.js:244).

**Contradictions:** Events suppresses sampled latency at [server.c:3625](src/server.c:3625), while Queries unconditionally emits an average at [:3829](src/server.c:3829). Raw Queries averages full durations ([compute.c:1741](src/compute.c:1741)); summary Queries divides aggregated time by count ([:2736](src/compute.c:2736)). Suppress the latency average wherever equivalent full-duration evidence is absent. Lifecycle statistics also ignore the PID filter today (server.c:3772, 3793): under a PID drill, suppress them until scoped correctly; class/event filters must explicitly leave execution timing whole-query.

**Single fixture:** extend [test_data_queries.py:19](tests/test_data_queries.py:19) with two 10 ms executions, each containing 2 ms CPU + 8 ms IO. Expect four intervals, 5 ms average interval, two executions, 10 ms average execution; include sampled and PID-filtered variants. **Negative control:** substitute interval count for execution count, or expose sampled average as latency; reject both.

4. **Histogram saturation — #437**

**Contract: exact visible sentence:**

> “Percentiles marked ≥16.4 ms fall in the open-ended ≥16.384 ms histogram bucket; they are lower bounds, and their actual values are unavailable.”

Show it beside the affected table, not only on hover. Keep per-cell `≥` driven by overflow flags. Never infer overflow from `16384` alone: the preceding finite bucket returns the same number. Max is the observed maximum, not a recovered percentile.

**Already correct:** bucket semantics and flags at [compute.c:1375](src/compute.c:1375), rendering at [table-configs.js:86](web/static/lib/builders/table-configs.js:86), and the requirement at [VISUAL_CHECKLIST.md:127](docs/VISUAL_CHECKLIST.md:127). Replace the tooltip wording at [table-configs.js:82](web/static/lib/builders/table-configs.js:82): “the true value is higher” incorrectly excludes equality. **This is a wording/visibility job; no histogram redesign is required.**

**Single fixture:** existing [events-overflow-pctl](web/static/dev/fixtures/table-configs.mjs:143), extended to assert the visible sentence and the finite-bucket control. **Negative control:** remove an overflow flag or hide the sentence; the semantic/render check must fail.

5. **Transitions and simplification — #436, #438**

**Contract:** An edge A→B counts exact records whose `old_event=A`, `new_event=B`; edge duration sums the **full source-state dwell**, not target latency or time between unrelated waits. CPU* is a literal intermediate state, not necessarily measured running CPU. Never contract A→CPU*→B into A→B. Sample-only transition data is unavailable.

Preserve the existing DFG criterion: after the declared idle exclusion, retain edges with `count ≥ maximum remaining edge count × threshold/100`; default threshold is 20%. This ranks **frequency**, not DB Time. Matrix retains its top-20 selection by summed incoming/outgoing counts. Record these definitions beside [transitions.js:18](web/static/lib/builders/transitions.js:18) and [matrix.js:52](web/static/lib/builders/matrix.js:52).

Disclose shown/total transition counts and separately identify idle exclusion, threshold exclusion and server truncation. Name omitted dominant-duration waits and provide a working restore action. “Show all” must remove client exclusions and fetch omitted links, or explicitly say it restores only returned links. Nodes, labels and complete arcs must fit the rendered viewport.

**Already correct:** literal edges at [compute.c:2875](src/compute.c:2875), thresholding at [transitions.js:95](web/static/lib/builders/transitions.js:95), and Matrix omission accounting at [matrix.js:118](web/static/lib/builders/matrix.js:118). CPU*-centered topology itself needs explanation, not a new metric.

**Contradictions:** graph nodes accumulate unfiltered/sample-inclusive records at [server.c:4868](src/server.c:4868), unlike exact, filtered edges at compute.c:2880–2885. Align their populations. Also, `hideIdle` recognizes only ClientRead ([transitions.js:32](web/static/lib/builders/transitions.js:32)), contradicting the pacing-sleep rule; either use authoritative classification or label the control specifically “Hide ClientRead”.

**Single fixture:** extend [threshold-simplified](web/static/dev/fixtures/transitions.mjs:185) with a rare but longest-duration Lock:relation, CPU-mediated paths, pacing idle and server truncation; check both views and restored geometry. **Negative control:** omit Lock:relation without disclosure, or synthesize a direct wait→wait edge; reject either.

6. **Refresh ordering and near-ties — #440**

**Contract — proposed new policy:** First render orders table rows by descending contribution, then stable identity. On subsequent refreshes, retain previous order for near-ties: a lower row passes its predecessor only when its current value is **strictly greater than 1.05×** the predecessor’s. Apply deterministic left-to-right adjacent passes until no qualifying inversion remains. Remove departed identities; append newcomers in contribution/identity order before those passes. Reset on window/filter change or explicit re-sort. Apply within Overview sibling groups, preserving hierarchy.

Explicit column sorting uses exact displayed values with identity tie-breaks; unavailable values remain last. Selection/clicks follow identity, never index. The 5% threshold is a draft policy choice, **not an existing repository rule**.

Record it at [VISUAL_CHECKLIST.md:23](docs/VISUAL_CHECKLIST.md:23) and [table.js:30](web/static/lib/table.js:30). Today tables preserve incoming order by default, and backend comparators lack identity tie-breaks ([compute.c:1413](src/compute.c:1413), [:1552](src/compute.c:1552), [:1660](src/compute.c:1660)). **Already correct:** AAS orders series by identity ([aas.js:101](web/static/lib/builders/aas.js:101)); Matrix preserves surviving axis positions ([matrix.js:83](web/static/lib/builders/matrix.js:83)). Keep those chart policies.

**Single fixture:** add `refresh-near-ties` beside [table.test.mjs:74](tests/web_unit/table.test.mjs:74): shuffled identical input, alternating 100/101 values, then 100/106, selected-row deletion and explicit re-sort. Expect no jitter, then a material reorder, with identity preserved. **Negative control:** replace the policy with exact descending sorting every tick; the alternating sequence must fail.
EXIT=0
