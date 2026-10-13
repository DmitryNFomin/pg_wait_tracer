# Capture budget and frame-spacing audit finding (2026-10-14)

No capture-budget behavior or threshold was changed. No counted attempt was run.

Source: `/Users/dmitryfomin/work/git/pg_wait_tracer/.claude/worktrees/agent-a3906538351ab35ea/tests/results/demo_rehearsal/summary.json`; `run.id` = `1791907034`.
Summary SHA-256: `884a635d45ec5ff1aa5f36fcfe39003a43ef19e6003c7025881ad1cd5394d90f`.

`capture_ms_total_ms` sums the recorded screenshot wall-clock costs for the five
frames in a tick/sweep (`build_sweep_tick_record`, `tests/ui_live_smoke_lib.py`).
It is not a single-frame latency or the achieved spacing between frames.

Queue item 1.1 (`docs/DEMO_DELIVERY_QUEUE.md:30`) explicitly registers it as
**reporting-only**, retaining the historical 800 ms diagnostic comparison.
`CAPTURE_MS_BOUND_MS` documents the calibration error: the cited ~600 ms regression
regime was actually healthy DPR-1 cost; unmodified healthy master measured
374–1156 ms/sweep. Thus 800 ms is not a registered acceptance budget. These
exceedances are capture-cost findings, not proof of an acceptance-budget breach.
No replacement budget or spacing tolerance is selected here.

| Pass | Tabs over 800 ms | Sweeps over 800 ms | All sweeps min–max (ms) | Maximum location |
|---|---:|---:|---:|---|
| early | 8/11 | 39/66 | 434.1–1452.5 | sessions tick 5 |
| middle | 9/11 | 40/66 | 425.6–1379.9 | events tick 5 |
| late | 9/11 | 39/66 | 380.1–1849.3 | queries tick 2 |

118/198 sweeps exceeded the diagnostic value. 25/26 tabs flagged by capture
budget still had `ok=True`; late/histogram was independently red for a blink
ratio of 0.0481710864 against 0.001. The capture-budget/spacing fields did not
gate any of these tab verdicts. The retained run itself was red, not clean.

| Pass/tab with `capture_budget.ok=False` | Maximum sweep cost (ms) |
|---|---:|
| early/concurrency | 835.4 |
| early/events | 1102.8 |
| early/matrix | 960.9 |
| early/overview | 1204.8 |
| early/queries | 1350.9 |
| early/scatter | 1155.9 |
| early/sessions | 1452.5 |
| early/transitions | 973.4 |
| middle/concurrency | 946.1 |
| middle/events | 1379.9 |
| middle/histogram | 879.4 |
| middle/matrix | 1009.1 |
| middle/overview | 1120.3 |
| middle/queries | 1283.7 |
| middle/scatter | 1029.5 |
| middle/sessions | 1313.0 |
| middle/transitions | 977.0 |
| late/concurrency | 882.5 |
| late/events | 1218.3 |
| late/histogram | 1117.6 |
| late/matrix | 923.8 |
| late/overview | 964.5 |
| late/queries | 1849.3 |
| late/scatter | 1174.7 |
| late/sessions | 1317.8 |
| late/transitions | 900.4 |

Frame spacing remains reporting-only; queue item 1.1 was corrected.
The original row describes the implementation, rather than creating a new
acceptance rule: `docs/DEMO_DELIVERY_QUEUE.md:30` says **"As shipped:"** and
**"A document must not promise a threshold the code does not enforce"**.
The criteria doc also records the earlier calibration finding (lines 69–72):

> calibration edge on the tab's own tick-1 warm-up transient, reproduced
> on an unmodified tree with no workload change at all

That descriptive queue claim conflicts with the later, explicitly approved
implementation demotion. The decision also uses the existing demotion text beside `FRAME_SPACING_DRIFT_BOUND_MS`
in `tests/ui_live_smoke_lib.py` (base `origin/master`, lines 1296–1307 and 1312–1319):

> DEMOTED TO REPORTING-ONLY (owner-approved).
>
> No scalar drift threshold separates those two populations.

The documented deliberate size regression scored 158 ms; loaded healthy capture
scored 297 ms. The same comment names issues #303/#304 as successors and
`frame_dims_ok` as the direct size-defect detector. The 150 ms diagnostic stays
unchanged. This run reports spacing failures on late/matrix, late/queries,
middle/histogram and middle/matrix, matching the audit. Reinstating that
non-discriminating diagnostic as a gate would contradict the documented demotion.

Live-tier exercise still needed: per-tab first paint, degraded/LEGACY tier refusal
and real measured-CPU availability, and achieved first sweep offsets. The owner
will route that run; no box run was made here. Capture budget needs inspection
on that run, with physical cost retained, without a new acceptance threshold.
