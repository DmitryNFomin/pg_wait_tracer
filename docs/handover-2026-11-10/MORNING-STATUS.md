# Status — the nine morning defects, and what the day added

2026-10-16. Nothing is merged yet, so **every issue below is still formally OPEN**; "fixed"
means the code is written, reviewed, reworked after an adviser review, and sitting on a branch
awaiting integration.

## The nine from this morning's UI review

| # | What it was | State |
|---|---|---|
| **#437** | P50/P95/P99 print `>=16.4ms` for second-scale waits; row contradicts its own avg/max | **Fixed** — exact disclosure sentence beside the table, per-cell `>=` driven by overflow flags. No histogram redesign needed. |
| **#438** | Transition Matrix is a degenerate CPU\*-centred L | **Fixed** — CPU\* labelled as the literal intermediate state it is. Topology was mathematically correct; the fix is explanation, not a new metric. |
| **#439** | Sessions counts background DB Time, Overview excludes it | **Fixed** — turned out to be *labelling*, not exclusion. Background work legitimately belongs in total DB Time; Queries' narrower denominator is now labelled "% attributed DB Time". |
| **#415** | Drilled Waterfall readout 67–101px below the fold | **Fixed** — existing fix integrated, verified in the real app shell at 1710x981, and wired into three runners (it was previously a check nothing ran). |
| **#435** | Idle renders as `0.0%` of DB Time beside 22,122s | **Fixed** — both halves. Backend emits JSON null; UI renders `—` with "Excluded from DB Time". |
| **#436** | Transitions clips upper links; dense labels unreadable at GIF scale | **Fixed** — both halves. Root cause was ECharts' graph series auto-fitting the node bounding box, so computed margins were never rendered margins. |
| **#440** | Near-equal rows swap between ticks | **Fixed** — 1.05x hysteresis, wired into **all four** table views (first attempt covered only Overview). |
| **#441** | Queries row reads as a contradiction | **Fixed** — both halves. Columns now distinguish wait/CPU intervals, executions, and accumulated time; a latency average is suppressed where the evidence for it is absent. |
| **#442** | Concurrency burst markers fire on nearly every bucket | **Diagnosed, stays OPEN, does not block** — see below. |

### #442 in full, because it is the one that stays open

The detector defines a burst as **"4+ sessions within 10ms"** — an *absolute* threshold. Under
pgbench with >=4 clients that is true in nearly every bucket by construction, so the marker has
no false-negative capacity and carries no information. The arithmetic is right; the **contract**
is wrong. "Detector correct, workload bursty" is the defect restated, not a clean bill.

It does not block the tag because **neither published GIF contains the Concurrency tab** —
the headline GIF is Overview-only, the drill-down GIF is Waterfall-only (verified in the frame
selection, not assumed). The eleven-tab walk visits it for the video, which is reviewed, not posted.

Release line: *"#442 remains open: Concurrency's burst markers are uninformative under the
reported pgbench load; neither published GIF includes Concurrency."*

This was also my scoping miss — I read an adviser's "conditional" as "non-blocking" and never
assigned it until the status question was asked.

## New issues filed today

| # | What |
|---|---|
| **#443** | `.gitignore` rules fell behind agent-written evidence, so 20 untracked results directories (plus a stray 56 MB directory from a botched `scp`) sat inside the candidate tree hash — the exact failure that disqualified counted rehearsal attempt 2 |
| **#444** | `test_web_ui.py` synchronises with a fixed `wait_for_timeout(200)` in 5 places instead of an explicit paint signal — pre-existing; a bet on runner speed, not a synchronisation primitive |
| **#445** | `test_capture_smoke.py` / `test_uprobe_fired.py` run only in CI, so `box-check` never exercises the capture phase — found by the new #433 guard on its first real run |
| **#446** | Four test files on disk are invoked by **no** runner at all — found by the new orphan scan, deliberately left unexempted so the guard stays a true positive |
| **#447** | A delayed seal could write records into a file whose header start is *after* them, and the reader prunes such files — so correctly captured records went invisible to the window containing them |
| **#448** | Two residual rotation-boundary hazards left open by the #430/#447 fix: the span-<=1h argument fails for a far-displaced replay timeline, and a >=boundary second can still land in the closing file |

## Also fixed today, not from the morning list

**#430** — hourly rotation paused open crediting. Worse than filed: a capture ending inside the
pause wrote ~6000 of 11000 ms **nowhere** (absent, not misattributed). Rotation is hourly on the
wall clock and the demo capture is 35 minutes, so whether the demo hit it was decided purely by
its start minute. Fixed in `flush_accum` — the single funnel every record passes — so it holds
for push, seal and rotation. Ten mutants, ten red.

**#431 / #433 / #426** — a guard that approved while blind; two test runners whose membership
nobody reconciled; AAS top-N ranking by unclipped duration.

## What is not done

- **Nothing is merged.** Integration is running the five-branch tree now.
- **Two consecutive clean counted rehearsals** are required on the final sha. Attempts 3 and 4
  were clean on a *different* tree; per `docs/DEMO_REHEARSAL_CRITERIA.md:1175`, landing a merge
  resets the counter to zero and "the tree tagged must be the tree rehearsed twice". My earlier
  plan of one attempt was wrong.
- **Video analysis by both advisers**, then the GIFs.
- **Owner's two signed Mac walks** — Mac-side is 0 of 2 and only the owner's signature advances
  it; each capture attempt needs its own paired walk on the same sha.

## One thing a clean rehearsal will NOT prove

No rehearsal assertion is guaranteed to go red if #430 or #435 were silently still broken. The
cross-tab comparisons have no independent oracle — both endpoints read the same records, so a
common missing input moves both sides together and still prints a zero gap. Those two fixes rest
on their targeted unit tests (`test_conservation_no_push`, `test_overview_idle_share_is_null`),
and the rehearsal verdict must say so rather than implying more.
