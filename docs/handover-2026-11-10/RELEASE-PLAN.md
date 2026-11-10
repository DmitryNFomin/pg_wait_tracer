# Release plan to a tagged demo

Written by **astra** (`gpt-6-astra`) on 2026-10-16, in answer to the owner's request:
*"Ask Astra to write plan to fix all of them and run all tests rehearsals, and video."*

Requested after nine UI defects (#435–#442, #415) surfaced in a single pass over eleven tabs.
The brief asked explicitly for a way to **stop discovering defects one review at a time** —
not another task list.

Sections 1–3 below are astra's text verbatim. The "Earlier answers" section at the end
summarises its two prior replies (already relayed to the owner verbally) in my words.

---

## SECTION 1 — THE SYSTEMATIC SWEEP

**Run one structured audit of the entire interface, backed by permanent regression checks—not another informal tour of the tabs.** No finite audit guarantees every possible defect, but this can systematically expose the six classes found tonight and make omissions explicit.

The audit’s output should be a completed coverage matrix, a defect ledger, and evidence showing that each automated check detects a deliberately broken case.

### 1. Define the coverage before running the audit

Enumerate all eleven tabs explicitly:

**Overview, Events, Sessions, Queries, Histogram, Timeline, Transitions, Concurrency, Waterfall, Scatter, Matrix.**

For each, register the seven states: **empty, single-row/entity, dense, drilled, filtered, mid-refresh, error**. That starts with 77 coverage entries, not necessarily 77 independent fixtures.

Each entry must name:

- The fixture and expected records, identities, values, and visible marks.
- The interaction that reaches it, including inherited filters and breadcrumbs.
- The expected explanation when the view cannot provide that information.
- The automated assertions and the human review evidence.

“Not applicable” requires a reason. An unsupported drill must have its unavailable behavior checked; it cannot silently disappear from coverage.

Exercise the actual application shell as well as isolated builders. A gallery cannot catch a Waterfall pushed below the fold by breadcrumbs, controls, and the persistent AAS chart.

Run at the intended presentation viewport and a smaller supported viewport, with a larger-text check. Include long labels, expanded controls, tooltips near edges, and a full drill breadcrumb. Test initial entry, return from another tab, filter removal, and refresh while drilled: identical-looking states can arrive through different broken paths.

### 2. Use fixtures that force the defects to exist

The fixture corpus needs three complementary layers:

| Layer | Required coverage | What it establishes |
|---|---|---|
| Hand-calculated traces | Known durations, populations, identities, windows, counts and expected aggregates | An independent numerical oracle |
| Deterministic UI responses | Dense layouts, long labels, nulls, overflow flags, delayed/reordered responses, explicit errors | Reliable reproduction of rendering and interaction failures |
| Frozen real capture | Tonight’s failing data, exercised through the real reader, server and browser | The real failure survives the test harness |

Do not derive expected results by calling the production reducer being tested. Likewise, a mocked response proves presentation behavior, not backend correctness.

The minimum adversarial corpus should contain:

1. **Idle plus foreground and background activity.** Large idle duration, nonzero foreground DB Time, known background time, and an idle-only case. This exposes #435 and #439 and provides an independent check for #425.
2. **Window-straddling records.** Activity beginning before the window, ending after it, crossing both boundaries, and landing exactly on boundaries. Include an event whose full duration ranks first but whose in-window contribution does not: the discriminator for #426.
3. **Latency distributions across the histogram ceiling.** Unsaturated, partly saturated, entirely saturated, zero observations, and known multi-second waits. Include the 3.9s-average case.
4. **Different query populations and measures.** Many short waits inside relatively few long executions, partial executions, missing execution timing, and the 720.4s / 91 executions / 36µs example. Expected labels must explain which population each measure describes.
5. **Transition topology extremes.** A CPU*-centered star, one edge, self-loops, dense edges, long labels, and a low-frequency but dominant-duration wait such as Lock:relation at 713s. Exercise simplify at zero, the default, 20%, and a high setting.
6. **Timeline and execution diversity.** One PID, many PIDs, one selected PID, overlapping intervals, dense coalescing, multiple execution lanes, and no executions despite available wait data.
7. **Concurrency contrast.** Quiet activity, steady high activity, one synchronized onset, repeated legitimate onsets, and nearly every bucket meeting the detector’s literal threshold.
8. **Refresh sequences.** Identical snapshots with shuffled input order; near-ties that oscillate; genuine rank changes; deletion of the selected row; delayed and out-of-order responses; error followed by recovery.
9. **Capture modes and lifecycle boundaries.** Exact versus sampled data, unavailable detail, completed versus open intervals, and rotation where the tag claims support.

These fixtures must also drive Histogram and Scatter. Their lack of reported defects tonight is not evidence that they were exercised adequately.

### 3. Permanently gate the automatable parts—and prove each can fail

Every check below needs a positive case and a named negative control. A negative control passes its *meta-test* only when the normal checker rejects it for the expected reason. A startup crash is not evidence that a semantic assertion works.

| Permanent check | Assertions | Required proof that it can fail |
|---|---|---|
| **Coverage and execution** | Expected tabs and state entries executed; expected fixture loaded; required assertions ran; required dependencies present. | Remove one tab, skip a fixture, return zero discovered cases, and remove a required dependency. Each must make the release job non-green. |
| **Visibility and occlusion** | Required content exists, has nonzero dimensions, lies within its intended clip region, and is reachable. Primary readouts meet the specified initial-viewport requirement. Include labels, curved edges and tooltips—not just node centers. | Move a node off-canvas, clip an arc, cover a control, and push the drilled Waterfall readout down by roughly tonight’s 100px. Each must trigger its geometry or visual assertion. |
| **Visual regression** | Full-shell and focused screenshots match independently reviewed fixtures under pinned rendering conditions. Mask only truly volatile content. | Hide the largest wait, erase a lane, or clip a label. The configured comparison must reject each change; otherwise its tolerance is too loose. Missing baselines must fail. |
| **Metric semantics** | Labels, units, denominators, populations and missing-value states match the contract. Idle is not represented as a meaningful 0% of DB Time; background time is not silently mixed into foreground DB Time. | Inject numeric zero for not-applicable idle share, include background time in one foreground total, and relabel average wait as average execution. Each must fail. |
| **Cross-tab reconciliation** | At one frozen window/filter/refresh generation, comparable aggregates agree within declared rounding or sampling bounds. AAS integrates to the corresponding DB Time; equivalent event and session populations reconcile. | Alter one tab’s time, population or filter while leaving the others unchanged. The reconciliation must fail and name the mismatch. |
| **Saturation and arithmetic** | Percentile bounds remain bounds; saturation is visibly explained; unavailable statistics remain unavailable. Arithmetic identities apply only to matching populations and window semantics. | Remove an overflow flag, present the top bucket as an exact percentile, change a unit by ×1,000, or substitute execution count for wait count. Each must fail. |
| **Ordering and refresh integrity** | Identical data has deterministic order; default near-tie behavior follows an explicit stability policy; material changes still update. Selection follows identity. Stale responses cannot overwrite the current window. | Randomize tie order, force near-tie oscillation, attach selection to row position, and deliver an old response last. Each must fail its sequence assertion. |
| **Topology and information retention** | Expected entities and lanes survive rendering; aggregation is declared; simplification reports what it excludes and has a working restore path. CPU*-mediated transitions are identified correctly. | Drop a known PID, collapse a multi-PID fixture to an unlabeled single lane, remove Lock:relation without the required disclosure, or erase a known edge. Each must fail. |
| **Burst meaning** | Detector output matches independently calculated onsets; displayed markers correspond to those onsets; labels describe the detector’s actual meaning. | Mark every bucket in the quiet fixture, duplicate one onset, and suppress the isolated real burst. Each must fail. Do not impose an arbitrary “few markers” rule on legitimately dense activity. |
| **Errors and unavailable data** | Empty, loading, stale, unsupported and failed states are distinguishable; recovery works; old numbers are not presented under a new window as current. | Swallow a server error, leave “Loading” indefinitely, substitute an empty success for an error, or retain stale rows without disclosure. Each must fail. |

For canvas charts, DOM bounds alone are insufficient. Use rendered-coordinate checks where available **plus** screenshots with negative controls; checking an ECharts option object does not prove its contents were painted visibly.

The same discipline applies to the infrastructure gates:

- **#431:** remove, corrupt or stale the evidence the guard requires; point it at the wrong tree. It must refuse approval.
- **#433:** remove a required test from one runner and create a stale membership entry. The manifest comparison must fail. Different environments may have different declared test sets; unexplained differences are the defect.
- **#432:** where an exclusion or overlay fence is relied upon, deliberately activate the forbidden path. The fence must detect it.

Each evidence record should identify the candidate SHA, fixture hash, environment, case IDs, observed populations, assertion counts, and negative-control result. Zero observations is valid only for an explicitly empty fixture whose empty-state assertions actually ran.

### 4. Reserve human judgment for what assertions cannot establish

An independent reviewer must inspect the full coverage matrix, including short refresh sequences. The author can explain the implementation; the author’s own “looks good” is not the acceptance evidence.

Human review must answer:

- Can a viewer identify the dominant wait and follow its relationship across tabs?
- Do names and adjacent numbers suggest the correct interpretation without an author explaining them?
- Does the Matrix communicate anything useful, even if its CPU*-centered shape is mathematically correct?
- Are dense labels, curves, colors, tooltips and controls readable together?
- Does refreshing feel stable, and can someone click what they intended?
- Does an empty or unavailable view explain itself?

Automation can enforce a chosen layout or wording. It cannot establish that those choices communicate effectively to a new viewer.

**“Run once” should mean one complete discovery campaign with all findings triaged together.** Fixes still require verification, and the final combined candidate requires the permanent gates. It must not mean one screenshot pass followed by unverified changes.

## SECTION 2 — ORDERING AND BATCHING

Use **one integration owner, one active `web/` branch, and parallel work organized around shared files**. Several implementers can work concurrently without several competing UI branches.

The local checkout differs from your stated baseline, so the paths below identify ownership areas; exact overlap should be checked against `97dca0c` before assignments.

### 1. Establish the contracts before implementation splits

Agree first on:

- Foreground DB Time versus idle and background populations.
- Window-clipped contribution versus full-event latency.
- Wait count versus execution count and their corresponding averages.
- Histogram saturation wording.
- Transition edge meaning, simplification criterion and omitted-information disclosure.
- Default refresh ordering behavior.

These decisions unblock several issues at once. Implementers must not independently choose incompatible meanings for adjacent tabs.

### 2. Assign work by collision domain

| Workstream | Items | Shared files / dependency | Scheduling |
|---|---|---|---|
| Gate integrity | #431, #433; audit coverage and negative controls | Guard/check scripts, runner manifests, CI configuration and test infrastructure | Parallel with product fixes; one owner integrates shared runner changes. |
| Time and population correctness | #425, #426, backend portion of #439 | `src/compute.c`, potentially `src/server.c`, data tests and shared fixtures | One owner or explicitly serial patches. Separate issue numbers do not make these independent. |
| Rotation accounting | #430 | Producer loop, open-credit state, summary rotation/writer code | Parallel with UI work; coordinate with #425 if they touch the same accounting state. |
| Table presentation | #435, #437, #439, #440, #441 | `web/static/lib/builders/table-configs.js`, shared formatting/table code, view adapters; #440 may also involve server ordering | One coherent batch on the single web branch. Depends on the semantic contracts and relevant backend fixes. |
| Graph and matrix presentation | #436, #438 | Transition/matrix builders and views; shared transition payload and meanings | Follow the table batch on that same branch. Prepare fixtures and proposed acceptance criteria in parallel. |
| Remaining visual behavior | #415, #442, Timeline investigation | Waterfall, Concurrency and Timeline builders/views; possibly shared shell/CSS and backend concurrency reducer | Serial web batches. Review the existing #415 fix rather than rewriting it. |
| Documentation honesty | #428 and declared limitations | Documentation and relevant UI wording | Documentation can proceed independently; UI wording joins the web queue. |
| Deferred architecture | #427, live-edge overlay, summary open-CPU producer, D2 producer | Shared window/reducer contracts, producer lifecycle and persisted formats | Keep outside the tag’s implementation stream unless the chosen demo scope requires them. |

For #437, **presentation honesty and histogram redesign are separate jobs**. Visible overflow explanation can ship without changing persisted histogram formats. Expanding buckets may touch producers, readers, summaries and compatibility; that is not a small UI patch.

For #438, do not combine a layout repair with silently changing the graph from literal transitions into inferred wait-to-wait relationships. That would introduce a different metric.

### 3. Use parallel capacity where it helps

A practical allocation for several Opus implementers is:

- One owns gate integrity and coverage enforcement.
- One owns backend time/population fixes.
- One owns the single web branch.
- One owns independent fixture/oracle review and rotation investigation, subject to actual file overlap.

Others may prepare tests outside `web/`, review patches, or calculate fixture expectations. **Only the designated web owner edits and integrates `web/` changes.** A separate gallery or UI-test-support branch touching `web/` would still violate the one-branch rule.

Use the two Linux gate boxes as follows:

- **Box A:** exclusive live capture, timing, sampling and rotation validation. No competing workload during measurements.
- **Box B:** deterministic replay, server/data checks, browser sweep and visual comparisons in the pinned environment.

Throwaway VMs handle dependency removal, malformed inputs, guard failure injection, process death and exploratory reproduction. They provide fault-injection evidence, not substitutes for the designated gate environment.

### 4. Keep integration and acceptance serial

The dependency order is:

**Contracts and fixtures → shared backend semantics → table presentation → graph/layout batches → integrated sweep and acceptance.**

Gate repair proceeds alongside those changes but must be complete before its green result is trusted.

Independent branches can be tested concurrently. Their results do not add up to acceptance of the merged candidate: the integration owner must validate the combined tree. An independent reviewer checks the negative controls and human-facing result before accepting the evidence.

## SECTION 3 — SCOPE FOR THE TAG

**The tag needs honest visible behavior and trustworthy evidence. It does not need every unfinished architecture project.** An issue may remain open when its affected capability is clearly bounded or unavailable. A known wrong number in the demonstrated path is not cured by a release-note footnote.

| Item | Demo-tag decision | Minimum acceptable treatment |
|---|---|---|
| **#435 Idle presentation** | **Blocks as displayed.** | Show not-applicable DB-Time share consistently. Suppress or explicitly distinguish idle occupancy from active-session AAS. |
| **#425 Idle magnitude** | **Blocks displaying an unverified idle magnitude.** | Correct and independently verify it, or suppress that magnitude and state that idle accounting is unavailable. Cosmetic consistency alone is insufficient. |
| **#426 AAS ranking** | **Blocks windowed ranking claims.** | Rank by the in-window contribution and prove it with a rank-reversing boundary fixture. |
| **#436 clipped graph / missing dominant wait** | **Blocks the current default graph experience.** | Fix clipping. Preserve access to the dominant wait and make the simplification criterion and omitted contribution clear. Full restoration must work. |
| **#437 saturated percentiles** | **Blocks misleading presentation; histogram expansion can remain open.** | Explain saturation visibly in the affected view. Values remain lower bounds, not measured multi-second percentiles. |
| **#438 CPU*-centered Matrix** | **Can remain open.** | Label the literal CPU*-mediated transition semantics. Do not sell it as direct wait-to-wait causality. A more informative alternative visualization can wait. |
| **#439 background DB Time** | **Blocks unexplained population mismatch.** | Align the default population or clearly separate background activity and its totals. Equivalent views must reconcile. |
| **#440 near-equal row swapping** | **Usually nonblocking polish.** | Can remain if identities and clicks stay correct and the demo can use a stable paused or explicit sort. Selection moving to another entity is blocking. |
| **#441 query numbers** | **Blocks ambiguous labels, not the underlying correct metrics.** | Clearly distinguish wait average, execution count, execution timing and accumulated time. Do not “fix” correct arithmetic by conflating populations. |
| **#442 frequent burst markers** | **Conditional.** | Incorrect detection or claims of anomaly significance block. A correct literal threshold detector may remain, with its meaning clear and dense markers made readable or disabled. Adaptive anomaly detection can wait. |
| **#415 drilled Waterfall below fold** | **Blocks the intended drill demonstration.** | Integrate and verify the existing fix in the full application shell at the presentation viewport. |
| **Timeline single lane** | **Investigate before deciding.** | One selected PID or explicitly labeled aggregation is valid. Silently losing multiple expected lanes blocks; do not invent extra lanes for appearance. |
| **#427 boundary-semantics migration** | **Full migration can remain open.** | Bound the demo to verified window/mode behavior and disclose remaining boundary differences. Any known contradiction in the actual demonstrated path still blocks. |
| **#428 nonexistent exporter claim** | **Blocks the claim only.** | Correct the documentation. Building an exporter is unnecessary for this tag. |
| **#430 rotation pauses open crediting** | **Blocks a live demo that exercises rotation.** | Fix and validate it, or explicitly scope the demo to completed captures or a bounded capture that does not cross rotation. No claim of continuous accounting across rotation. |
| **#431 blind approving guard** | **Blocks trusting release approval.** | Make missing, stale or unreadable evidence fail closed and demonstrate rejection with negative controls. |
| **#432 latent overlay bypass** | **May remain dormant, subject to a proven fence.** | Establish that the tag cannot reach the bypass. If an active demonstrated path can reach it, it blocks. |
| **#433 runner membership** | **Blocks the claimed gate coverage.** | Reconcile required membership and explicit environment exclusions. Prove that omission of a required check fails. |
| **Live right edge / parked overlay** | **Can remain unfinished.** | Present results as subject to completed-record accounting; do not claim that all open work is continuously included. Keep the parked overlay out. |
| **Summary open-CPU producer / v9** | **Can remain unfinished.** | Keep version 8 and avoid claiming open-CPU summary support. A writer without a producer is not a delivered capability. |
| **D2 crash checkpoints** | **Can remain unfinished.** | Make no checkpoint-backed crash-recovery claim. Normal completed-capture demonstration does not require the producer. |

For that bounded tag, a presenter could truthfully say:

> “This demo covers the verified capture and window paths; histogram tails are lower bounds, transitions include CPU* intermediates, and live-edge, boundary, rotation, and crash-recovery limitations remain.”
tokens used

---

## Earlier answers from the same adviser (summarised, not verbatim)

**Critical path.** 31–46 staffed hours of implementation and verification; ~40–56 once one
new finding is assumed, i.e. **5–7 working days** at the parallelism this project actually
sustains (three active implementers, two gate boxes, one `web/` branch).

**Rehearsal cap.** The four-attempt cap is spent (2 consecutive clean of 4). astra recommends
the owner **amend it to four more counted attempts, numbered 5–8**, rather than tagging on
attempts whose tree predates nine known UI defects. Mac-side signed walks remain 0 of 2 and
cannot be advanced by any automated result.

**Workload.** astra backs the owner's standing no-workload-change rule without qualification:
a defect that only appears under the demo workload is a defect in the product.

**Media.** Video and GIFs come *after* the sweep closes, not before — a GIF cut from a tree
with #436 (clipped graph) or #415 (drilled Waterfall below the fold) records the defect
permanently and is the single most expensive artifact to redo.

**Closing line.** *"The owner should authorize a bounded release campaign whose first
milestone is complete coverage — not another evening of fixing the defects visible in the
last review."*
